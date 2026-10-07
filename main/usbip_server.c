#include "usbip_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "usb_backend.h"
#include "usbip_protocol.h"
#include "virtual_device.h"

static const char *TAG = "usbip";

/* Track active client connections for debugging FD exhaustion. */
static atomic_int active_connections;

static bool read_exact(int fd, void *buf, size_t len)
{
    uint8_t *ptr = (uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        const ssize_t n = recv(fd, ptr, remaining, 0);
        if (n <= 0) {
            ESP_LOGD(TAG, "read_exact: recv returned %zd (wanted %zu of %zu), errno=%d",
                     n, remaining, len, errno);
            return false;
        }
        ptr += n;
        remaining -= (size_t)n;
    }

    return true;
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        const ssize_t n = send(fd, ptr, remaining, 0);
        if (n <= 0) {
            return false;
        }
        ptr += n;
        remaining -= (size_t)n;
    }

    return true;
}

static bool discard_exact(int fd, size_t len)
{
    uint8_t scratch[128];
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = remaining;
        if (chunk > sizeof(scratch)) {
            chunk = sizeof(scratch);
        }

        if (!read_exact(fd, scratch, chunk)) {
            return false;
        }

        remaining -= chunk;
    }

    return true;
}

static uint32_t make_devid(const usbip_backend_device_t *device)
{
    return (device->busnum << 16) | (device->devnum & 0xFFFFu);
}

static void fill_wire_device_desc(const usbip_backend_device_t *src, usbip_device_desc_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    memcpy(dst->path, src->path, sizeof(dst->path));
    memcpy(dst->busid, src->busid, sizeof(dst->busid));

    dst->busnum = htonl(src->busnum);
    dst->devnum = htonl(src->devnum);
    dst->speed = htonl(src->speed);
    dst->id_vendor = htons(src->id_vendor);
    dst->id_product = htons(src->id_product);
    dst->bcd_device = htons(src->bcd_device);
    dst->device_class = src->device_class;
    dst->device_subclass = src->device_subclass;
    dst->device_protocol = src->device_protocol;
    dst->configuration_value = src->configuration_value;
    dst->num_configurations = src->num_configurations;
    dst->num_interfaces = src->num_interfaces;
}

static bool send_device_with_interfaces(int fd, const usbip_backend_device_t *device)
{
    usbip_device_desc_t wire_device;
    fill_wire_device_desc(device, &wire_device);

    if (!write_all(fd, &wire_device, sizeof(wire_device))) {
        return false;
    }

    for (uint8_t i = 0; i < device->num_interfaces; i++) {
        usbip_interface_desc_t intf = {
            .interface_class = device->interfaces[i].interface_class,
            .interface_subclass = device->interfaces[i].interface_subclass,
            .interface_protocol = device->interfaces[i].interface_protocol,
            .padding = 0,
        };
        if (!write_all(fd, &intf, sizeof(intf))) {
            return false;
        }
    }

    return true;
}

static bool send_op_common(int fd, uint16_t code, uint32_t status)
{
    usbip_op_common_t reply = {
        .version = htons(USBIP_VERSION),
        .code = htons(code),
        .status = htonl(status),
    };

    return write_all(fd, &reply, sizeof(reply));
}

static bool send_ret_submit(int fd,
                            const usbip_header_t *request,
                            int32_t status,
                            const uint8_t *payload,
                            uint32_t payload_len)
{
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));

    reply.base.command = htonl(USBIP_RET_SUBMIT);
    reply.base.seqnum = request->base.seqnum;
    reply.base.devid = request->base.devid;
    reply.base.direction = request->base.direction;
    reply.base.ep = request->base.ep;

    reply.u.ret_submit.status = htonl((uint32_t)status);
    reply.u.ret_submit.actual_length = htonl(payload_len);
    reply.u.ret_submit.start_frame = htonl(0);
    reply.u.ret_submit.number_of_packets = htonl(0);
    reply.u.ret_submit.error_count = htonl(0);
    reply.u.ret_submit.padding = 0;

    /* Send header and payload in a single TCP segment so the kernel
       can read both without waiting for a second segment. */
    if (payload_len > 0 && payload != NULL) {
        uint8_t *buf = malloc(sizeof(reply) + payload_len);
        if (buf == NULL) {
            return false;
        }
        memcpy(buf, &reply, sizeof(reply));
        memcpy(buf + sizeof(reply), payload, payload_len);
        bool ok = write_all(fd, buf, sizeof(reply) + payload_len);
        free(buf);
        return ok;
    }

    return write_all(fd, &reply, sizeof(reply));
}

/* RET_SUBMIT for an OUT URB: actual_length without a payload. */
static bool send_ret_submit_out(int fd, const usbip_header_t *request,
                                int32_t status, uint32_t actual_length)
{
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));

    reply.base.command = htonl(USBIP_RET_SUBMIT);
    reply.base.seqnum = request->base.seqnum;
    reply.base.devid = request->base.devid;
    reply.base.direction = request->base.direction;
    reply.base.ep = request->base.ep;

    reply.u.ret_submit.status = htonl((uint32_t)status);
    reply.u.ret_submit.actual_length = htonl(actual_length);

    return write_all(fd, &reply, sizeof(reply));
}

static bool send_ret_unlink(int fd, const usbip_header_t *request, int32_t status)
{
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));

    reply.base.command = htonl(USBIP_RET_UNLINK);
    reply.base.seqnum = request->base.seqnum;
    reply.base.devid = request->base.devid;
    reply.base.direction = request->base.direction;
    reply.base.ep = request->base.ep;

    reply.u.ret_unlink.status = htonl((uint32_t)status);

    return write_all(fd, &reply, sizeof(reply));
}


/* Maximum concurrent in-flight URBs per TCP connection. */
#define URB_STREAM_MAX_INFLIGHT 8

struct urb_work_item;

/* Per-connection context shared between the reader loop and worker tasks.
   write_mutex guards both socket writes and in_flight[]: a worker sends its
   reply and frees its slot under the lock, so CMD_UNLINK either finds the
   URB still pending or knows its RET_SUBMIT has already gone out. */
typedef struct {
    int fd;
    volatile bool cancel;                   /* set when client disconnects */
    SemaphoreHandle_t write_mutex;          /* serialises socket writes */
    atomic_int inflight_count;              /* active worker tasks */
    struct {
        struct urb_work_item *item;         /* NULL = slot free */
        uint32_t seqnum;
    } in_flight[URB_STREAM_MAX_INFLIGHT];
} urb_stream_ctx_t;

/* Work item handed to a worker task.  The worker does the blocking
   backend call, sends the response (under write_mutex), and frees
   all associated memory. */
typedef struct urb_work_item {
    urb_stream_ctx_t *stream;
    usbip_header_t request;
    char imported_busid[32];
    uint32_t expected_devid;
    volatile bool cancel;
    bool unlinked;                          /* CMD_UNLINK accepted for this URB */
    usbip_header_t unlink_request;          /* the CMD_UNLINK, to answer on completion */
    uint8_t *out_data;
    size_t out_len;
    uint8_t *in_data;
    size_t in_capacity;
    int slot;                               /* index in stream->in_flight[] */
} urb_work_item_t;

/* Free an in-flight slot so it can be reused.  Caller holds write_mutex. */
static void urb_stream_free_slot(urb_stream_ctx_t *ctx, int slot)
{
    ctx->in_flight[slot].item = NULL;
    ctx->in_flight[slot].seqnum = 0;
    atomic_fetch_sub(&ctx->inflight_count, 1);
}

/* Allocate an in-flight slot for item, return its index or -1 when full. */
static int urb_stream_alloc_slot(urb_stream_ctx_t *ctx, uint32_t seqnum,
                                  urb_work_item_t *item)
{
    int slot = -1;
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item == NULL) {
            ctx->in_flight[i].item = item;
            ctx->in_flight[i].seqnum = seqnum;
            atomic_fetch_add(&ctx->inflight_count, 1);
            slot = i;
            break;
        }
    }
    xSemaphoreGive(ctx->write_mutex);
    return slot;
}

/* Find an in-flight slot by seqnum (for CMD_UNLINK).  Caller holds write_mutex. */
static int urb_stream_find_by_seqnum(urb_stream_ctx_t *ctx, uint32_t seqnum)
{
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item != NULL
            && ctx->in_flight[i].seqnum == seqnum) {
            return i;
        }
    }
    return -1;
}

/* RET_SUBMIT from the reader loop, serialised against the workers. */
static bool stream_send_ret_submit(urb_stream_ctx_t *ctx,
                                   const usbip_header_t *request, int32_t status)
{
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    bool ok = send_ret_submit(ctx->fd, request, status, NULL, 0);
    xSemaphoreGive(ctx->write_mutex);
    return ok;
}

/* CMD_UNLINK, with Linux usbip-host semantics: if the URB is still pending,
   cancel it and answer RET_UNLINK(-ECONNRESET) *instead of* its RET_SUBMIT
   when it completes; if it has already completed (RET_SUBMIT sent), answer
   RET_UNLINK(0) now.  vhci_hcd drops the connection if a RET_SUBMIT arrives
   for a URB it has already given back. */
static bool stream_handle_unlink(urb_stream_ctx_t *ctx, const usbip_header_t *request)
{
    const uint32_t unlink_seq = ntohl(request->u.cmd_unlink.unlink_seqnum);
    bool ok = true;

    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    int slot = urb_stream_find_by_seqnum(ctx, unlink_seq);
    if (slot >= 0 && !ctx->in_flight[slot].item->unlinked) {
        urb_work_item_t *item = ctx->in_flight[slot].item;
        item->unlink_request = *request;
        item->unlinked = true;
        item->cancel = true;
        ESP_LOGD(TAG, "CMD_UNLINK seq=%"PRIu32": cancelling", unlink_seq);
    } else {
        ok = send_ret_unlink(ctx->fd, request, 0);
        ESP_LOGD(TAG, "CMD_UNLINK seq=%"PRIu32": already completed", unlink_seq);
    }
    xSemaphoreGive(ctx->write_mutex);
    return ok;
}

/* Does the blocking USB transfer, sends the response, and frees everything. */
static void urb_process(urb_work_item_t *item)
{
    urb_stream_ctx_t *ctx = item->stream;
    const uint32_t direction = ntohl(item->request.base.direction);
    const uint32_t endpoint = ntohl(item->request.base.ep);
    size_t in_len = 0;
    int status;

    /* Check if this is a virtual device. */
    virtual_device_t *vdev = virtual_device_find_by_busid(item->imported_busid);

    if (vdev != NULL) {
        if (endpoint == 0) {
            usb_setup_packet_t setup;
            memcpy(&setup, item->request.u.cmd_submit.setup, sizeof(setup));
            status = vdev->ops->control_transfer(vdev, &setup,
                                                  item->out_data, item->out_len,
                                                  item->in_data, item->in_capacity,
                                                  &in_len);
        } else {
            const uint8_t ep_addr = (uint8_t)(endpoint |
                (direction == USBIP_DIR_IN ? 0x80 : 0x00));
            status = vdev->ops->data_transfer(vdev, ep_addr,
                                               item->out_data, item->out_len,
                                               item->in_data, item->in_capacity,
                                               &in_len);

        }
    } else if (endpoint == 0) {
        usb_setup_packet_t setup;
        memcpy(&setup, item->request.u.cmd_submit.setup, sizeof(setup));
        status = usb_backend_control_transfer(item->imported_busid,
                                              &setup,
                                              item->out_data, item->out_len,
                                              item->in_data, item->in_capacity,
                                              &in_len,
                                              &item->cancel);
    } else {
        const uint8_t ep_addr = (uint8_t)(endpoint |
            (direction == USBIP_DIR_IN ? 0x80 : 0x00));
        if (usb_backend_is_interrupt_endpoint(item->imported_busid, endpoint,
                                              direction == USBIP_DIR_IN)) {
            status = usb_backend_interrupt_transfer(item->imported_busid,
                                                    ep_addr,
                                                    item->out_data, item->out_len,
                                                    item->in_data, item->in_capacity,
                                                    &in_len,
                                                    &item->cancel);

        } else {
            status = usb_backend_bulk_transfer(item->imported_busid,
                                               ep_addr,
                                               item->out_data, item->out_len,
                                               item->in_data, item->in_capacity,
                                               &in_len,
                                               &item->cancel);

        }
    }

    /* Reply and release the slot under write_mutex, so a concurrent
       CMD_UNLINK sees either a pending URB or one already answered. */
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    if (item->unlinked) {
        send_ret_unlink(ctx->fd, &item->unlink_request, -ECONNRESET);
    } else if (direction == USBIP_DIR_IN) {
        send_ret_submit(ctx->fd, &item->request, status,
                        status == 0 ? item->in_data : NULL,
                        status == 0 ? (uint32_t)in_len : 0);
    } else {
        /* OUT: actual_length is the number of bytes sent, with no payload.
           Clients check it (picotool treats a short PICOBOOT command write
           as a failure); the backend sends all or fails. */
        send_ret_submit_out(ctx->fd, &item->request, status,
                            status == 0 ? (uint32_t)item->out_len : 0);
    }
    urb_stream_free_slot(ctx, item->slot);
    xSemaphoreGive(ctx->write_mutex);

    free(item->out_data);
    free(item->in_data);
    free(item);
}

/* Task entry point for the real-device path. urb_process() must not delete the
   calling task: virtual devices run it inline on the connection task. */
static void urb_worker_task(void *arg)
{
    urb_process((urb_work_item_t *)arg);
    vTaskDelete(NULL);
}

static bool handle_urb_stream(int fd, const char imported_busid[32],
                               uint32_t expected_devid)
{
    /* Heap-allocate the stream context so worker tasks can safely
       access it even after this function starts unwinding.  The last
       worker to call urb_stream_free_slot() is responsible for
       freeing the context when the stream is shutting down. */
    urb_stream_ctx_t *ctx = calloc(1, sizeof(urb_stream_ctx_t));
    if (ctx == NULL) {
        return false;
    }
    ctx->fd = fd;
    ctx->write_mutex = xSemaphoreCreateMutex();
    if (ctx->write_mutex == NULL) {
        free(ctx);
        return false;
    }

    const bool is_virtual =
        (virtual_device_find_by_busid(imported_busid) != NULL);

    while (true) {
        usbip_header_t request;
        if (!read_exact(fd, &request, sizeof(request))) {
            ESP_LOGD(TAG, "URB stream: read failed (client disconnected?)");
            goto cleanup;
        }

        const uint32_t command = ntohl(request.base.command);

        if (command == USBIP_CMD_UNLINK) {
            if (!stream_handle_unlink(ctx, &request)) {
                goto cleanup;
            }
            continue;
        }

        if (command != USBIP_CMD_SUBMIT) {
            ESP_LOGD(TAG, "Unsupported USB/IP command: 0x%08"PRIx32, command);
            goto cleanup;
        }

        /* --- CMD_SUBMIT: validate and read out-data in the reader
               loop (preserves TCP ordering), then hand off to a
               worker task for the blocking backend call. --- */

        const uint32_t seqnum = ntohl(request.base.seqnum);
        const uint32_t devid = ntohl(request.base.devid);
        const uint32_t direction = ntohl(request.base.direction);
        const uint32_t endpoint = ntohl(request.base.ep);
        const int32_t req_len =
            (int32_t)ntohl((uint32_t)request.u.cmd_submit.transfer_buffer_length);
        const uint32_t packet_count =
            (uint32_t)ntohl((uint32_t)request.u.cmd_submit.number_of_packets);

        /* Quick validation — errors that don't need a worker. */
        if (req_len < 0) {
            ESP_LOGD(TAG, "  -> EINVAL (req_len < 0)");
            if (!stream_send_ret_submit(ctx, &request, -EINVAL))
                goto cleanup;
            continue;
        }
        if (req_len > CONFIG_USBIP_MAX_TRANSFER) {
            ESP_LOGD(TAG, "  -> EMSGSIZE");
            if (direction == USBIP_DIR_OUT && req_len > 0) {
                if (!discard_exact(fd, (size_t)req_len)) goto cleanup;
            }
            if (!stream_send_ret_submit(ctx, &request, -EMSGSIZE))
                goto cleanup;
            continue;
        }
        if (direction != USBIP_DIR_OUT && direction != USBIP_DIR_IN) {
            ESP_LOGD(TAG, "  -> bad direction");
            goto cleanup;
        }
        if (devid != expected_devid) {
            ESP_LOGD(TAG, "  -> ENODEV");
            if (direction == USBIP_DIR_OUT && req_len > 0) {
                if (!discard_exact(fd, (size_t)req_len)) goto cleanup;
            }
            if (!stream_send_ret_submit(ctx, &request, -ENODEV))
                goto cleanup;
            continue;
        }
        if (packet_count != USBIP_NON_ISO_PACKETS && packet_count != 0) {
            ESP_LOGD(TAG, "  -> EOPNOTSUPP (iso)");
            if (!stream_send_ret_submit(ctx, &request, -EOPNOTSUPP))
                goto cleanup;
            continue;
        }

        /* For EP0 control transfers, check direction consistency. */
        if (endpoint == 0) {
            const bool setup_in =
                (request.u.cmd_submit.setup[0] & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
            if (((direction == USBIP_DIR_IN) ? true : false) != setup_in) {
                if (!stream_send_ret_submit(ctx, &request, -EINVAL))
                    goto cleanup;
                continue;
            }
        }

        /* Read OUT data payload (follows the header in the TCP stream). */
        uint8_t *out_data = NULL;
        size_t out_len = (direction == USBIP_DIR_OUT) ? (size_t)req_len : 0;
        if (out_len > 0) {
            out_data = malloc(out_len);
            if (out_data == NULL) {
                if (!discard_exact(fd, out_len)) goto cleanup;
                if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                    goto cleanup;
                continue;
            }
            if (!read_exact(fd, out_data, out_len)) {
                free(out_data);
                goto cleanup;
            }
        }

        /* Allocate IN buffer. */
        uint8_t *in_data = NULL;
        size_t in_capacity =
            (direction == USBIP_DIR_IN) ? (size_t)req_len : 0;
        if (in_capacity > 0) {
            in_data = malloc(in_capacity);
            if (in_data == NULL) {
                free(out_data);
                if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                    goto cleanup;
                continue;
            }
        }

        /* Build work item and spawn worker. */
        urb_work_item_t *item = calloc(1, sizeof(urb_work_item_t));
        if (item == NULL) {
            free(out_data);
            free(in_data);
            if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                goto cleanup;
            continue;
        }

        item->stream = ctx;
        item->request = request;
        memcpy(item->imported_busid, imported_busid, sizeof(item->imported_busid));
        item->expected_devid = expected_devid;
        item->cancel = false;
        item->out_data = out_data;
        item->out_len = out_len;
        item->in_data = in_data;
        item->in_capacity = in_capacity;

        item->slot = urb_stream_alloc_slot(ctx, seqnum, item);
        if (item->slot < 0) {
            /* Too many in-flight URBs — wait for a slot, then retry.
               This is simpler than implementing a wait queue. */
            ESP_LOGD(TAG, "  -> too many in-flight, waiting");
            while (item->slot < 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                item->slot = urb_stream_alloc_slot(ctx, seqnum, item);
                if (ctx->cancel) {
                    free(out_data);
                    free(in_data);
                    free(item);
                    goto cleanup;
                }
            }
        }

        /* Virtual devices complete instantly, so we can run them
           inline and avoid the task creation overhead. */
        if (is_virtual) {
            urb_process(item);
        } else {
            if (xTaskCreate(urb_worker_task, "urb_wrk",
                            CONFIG_USBIP_SERVER_TASK_STACK, item,
                            CONFIG_USBIP_SERVER_TASK_PRIORITY, NULL) != pdPASS) {
                xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
                urb_stream_free_slot(ctx, item->slot);
                xSemaphoreGive(ctx->write_mutex);
                free(out_data);
                free(in_data);
                free(item);
                if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                    goto cleanup;
            }
        }
    }

cleanup:
    /* Signal all in-flight workers to abort, then wait for them. */
    ctx->cancel = true;
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    ctx->fd = -1;   /* the caller closes the socket; late replies must not reach a reused fd */
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item != NULL) {
            ctx->in_flight[i].item->cancel = true;
        }
    }
    xSemaphoreGive(ctx->write_mutex);
    /* Give workers time to wake up and exit. */
    for (int timeout = 0; timeout < 50; timeout++) {
        if (atomic_load(&ctx->inflight_count) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (atomic_load(&ctx->inflight_count) != 0) {
        /* A worker is still inside the backend and will touch ctx when it
           finishes; leak ctx rather than free it under the worker. */
        ESP_LOGW(TAG, "URB stream: %d worker(s) still busy, leaking context",
                 atomic_load(&ctx->inflight_count));
        return false;
    }
    vSemaphoreDelete(ctx->write_mutex);
    free(ctx);
    return false;
}

static bool handle_devlist_request(int fd)
{
    usbip_backend_device_t *devices = malloc((CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX) * sizeof(usbip_backend_device_t));
    if (devices == NULL) {
        ESP_LOGE(TAG, "DEVLIST: malloc failed");
        return false;
    }

    const size_t device_count = usb_backend_get_devices(devices, CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX);

    ESP_LOGD(TAG, "DEVLIST: reporting %zu devices", device_count);
    for (size_t i = 0; i < device_count; i++) {
        ESP_LOGD(TAG, "  [%zu] busid=%.32s vid=%04x pid=%04x class=%02x speed=%"PRIu32" intfs=%u",
                 i, devices[i].busid, devices[i].id_vendor, devices[i].id_product,
                 devices[i].device_class, devices[i].speed, devices[i].num_interfaces);
    }

    if (!send_op_common(fd, USBIP_OP_REP_DEVLIST, 0)) {
        free(devices);
        return false;
    }

    const uint32_t count = htonl((uint32_t)device_count);
    if (!write_all(fd, &count, sizeof(count))) {
        free(devices);
        return false;
    }

    for (size_t i = 0; i < device_count; i++) {
        if (!send_device_with_interfaces(fd, &devices[i])) {
            free(devices);
            return false;
        }
    }

    free(devices);
    return true;
}

static bool handle_import_request(int fd)
{
    char requested_busid[32];
    if (!read_exact(fd, requested_busid, sizeof(requested_busid))) {
        ESP_LOGD(TAG, "IMPORT: failed to read busid");
        return false;
    }

    ESP_LOGD(TAG, "IMPORT: requested busid='%.32s'", requested_busid);

    usbip_backend_device_t device;
    const bool found = usb_backend_get_device_by_busid(requested_busid, &device);

    ESP_LOGD(TAG, "IMPORT: device lookup %s (busid='%.32s')", found ? "FOUND" : "NOT FOUND", requested_busid);

    if (!send_op_common(fd, USBIP_OP_REP_IMPORT, found ? 0 : 1)) {
        ESP_LOGD(TAG, "IMPORT: failed to send reply header");
        return false;
    }

    if (!found) {
        ESP_LOGD(TAG, "IMPORT: device not found, closing");
        return true;
    }

    /* Import reply sends only the device descriptor, NOT the interface
       descriptors.  The Linux usbip userspace tool (usbip_attach.c)
       reads only sizeof(struct usbip_usb_device) — the interface
       descriptors in op_import_reply are commented out in the kernel
       source.  Any extra bytes left in the TCP buffer would be
       misinterpreted as URB PDU data by the kernel driver. */
    usbip_device_desc_t wire_device;
    fill_wire_device_desc(&device, &wire_device);
    if (!write_all(fd, &wire_device, sizeof(wire_device))) {
        return false;
    }

    ESP_LOGD(TAG, "Client imported %s", device.busid);
    return handle_urb_stream(fd, requested_busid, make_devid(&device));
}

static void handle_client(int fd)
{
    usbip_op_common_t request;
    if (!read_exact(fd, &request, sizeof(request))) {
        ESP_LOGD(TAG, "Failed to read op_common header (%d bytes)", (int)sizeof(request));
        return;
    }

    const uint16_t version = ntohs(request.version);
    const uint16_t code = ntohs(request.code);

    ESP_LOGD(TAG, "Received op: version=0x%04x code=0x%04x status=0x%08"PRIx32,
             version, code, (uint32_t)ntohl(request.status));

    if (version != USBIP_VERSION) {
        ESP_LOGD(TAG, "Unsupported USB/IP version 0x%04x (expected 0x%04x)", version, USBIP_VERSION);
        return;
    }

    if (code == USBIP_OP_REQ_DEVLIST) {
        ESP_LOGD(TAG, "Handling DEVLIST request");
        (void)handle_devlist_request(fd);
        return;
    }

    if (code == USBIP_OP_REQ_IMPORT) {
        ESP_LOGD(TAG, "Handling IMPORT request");
        (void)handle_import_request(fd);
        return;
    }

    ESP_LOGD(TAG, "Unsupported USB/IP op request 0x%04x", code);
}

typedef struct {
    int fd;
} client_task_ctx_t;

static void client_task(void *arg)
{
    client_task_ctx_t *ctx = (client_task_ctx_t *)arg;
    const int fd = ctx->fd;
    free(ctx);

    handle_client(fd);
    close(fd);

    const int remaining = atomic_fetch_sub(&active_connections, 1) - 1;
    ESP_LOGD(TAG, "Client disconnected (%d active connection%s)",
             remaining, remaining == 1 ? "" : "s");

    vTaskDelete(NULL);
}

static void usbip_server_task(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(USBIP_TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 4) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(TAG, "USB/IP server listening on TCP %d", USBIP_TCP_PORT);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            const int current = atomic_load(&active_connections);
            const char *errname = "UNKNOWN";
            switch (errno) {
                case 23: errname = "ENFILE (too many open files)"; break;
                case 24: errname = "EMFILE (too many open fds)"; break;
                case 11: errname = "EAGAIN"; break;
                case 22: errname = "EINVAL"; break;
                case  9: errname = "EBADF"; break;
                case 12: errname = "ENOMEM"; break;
                case 14: errname = "EFAULT"; break;
            }
            ESP_LOGD(TAG, "accept() failed: errno=%d (%s) — %d active connection%s",
                     errno, errname, current, current == 1 ? "" : "s");
            continue;
        }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        const int count = atomic_fetch_add(&active_connections, 1) + 1;
        ESP_LOGD(TAG, "Client connected (%d active connection%s)",
                 count, count == 1 ? "" : "s");
        if (count >= 8) {
            ESP_LOGD(TAG, "⚠ %d active connections — approaching FD limits!", count);
        }

        client_task_ctx_t *ctx = malloc(sizeof(client_task_ctx_t));
        if (ctx == NULL) {
            ESP_LOGD(TAG, "Failed to allocate client context");
            close(client_fd);
            const int remaining = atomic_fetch_sub(&active_connections, 1) - 1;
            ESP_LOGD(TAG, "Connection dropped (%d active connection%s)",
                     remaining, remaining == 1 ? "" : "s");
            continue;
        }
        ctx->fd = client_fd;

        if (xTaskCreate(client_task, "usbip_client",
                        CONFIG_USBIP_SERVER_TASK_STACK, ctx,
                        CONFIG_USBIP_SERVER_TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGD(TAG, "Failed to create client task");
            free(ctx);
            close(client_fd);
            const int remaining = atomic_fetch_sub(&active_connections, 1) - 1;
            ESP_LOGD(TAG, "Connection dropped (%d active connection%s)",
                     remaining, remaining == 1 ? "" : "s");
        }
    }
}

esp_err_t usbip_server_start(void)
{
    if (xTaskCreate(usbip_server_task,
                    "usbip_server",
                    CONFIG_USBIP_SERVER_TASK_STACK,
                    NULL,
                    CONFIG_USBIP_SERVER_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
