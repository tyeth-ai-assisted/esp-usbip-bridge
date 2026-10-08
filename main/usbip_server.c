#include "usbip_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
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

static bool write_all_flags(int fd, const void *buf, size_t len, int flags)
{
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        const ssize_t n = send(fd, ptr, remaining, flags);
        if (n <= 0) {
            return false;
        }
        ptr += n;
        remaining -= (size_t)n;
    }

    return true;
}

static bool write_all(int fd, const void *buf, size_t len)
{
    return write_all_flags(fd, buf, len, 0);
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

/* RET_SUBMIT/RET_UNLINK status values are Linux errno values, and
   ESP-IDF's newlib numbers some errno constants differently (ETIMEDOUT is
   116 there but 110 on Linux, where 116 is ESTALE).  Translate the ones a
   URB status can carry; anything else becomes -EIO. */
#define USBIP_LINUX_EPROTO      71
#define USBIP_LINUX_EOVERFLOW   75
#define USBIP_LINUX_EILSEQ      84
#define USBIP_LINUX_EMSGSIZE    90
#define USBIP_LINUX_ECONNRESET  104
#define USBIP_LINUX_ESHUTDOWN   108
#define USBIP_LINUX_ETIMEDOUT   110
#define USBIP_LINUX_EINPROGRESS 115

/* These are passed through, so newlib must agree with Linux on them. */
_Static_assert(ENOENT == 2 && EIO == 5 && ENXIO == 6 && EAGAIN == 11 && ENOMEM == 12
               && EBUSY == 16 && EXDEV == 18 && ENODEV == 19 && EINVAL == 22
               && ENOSPC == 28 && EPIPE == 32 && ENOSR == 63 && ECOMM == 70
               && EPROTO == USBIP_LINUX_EPROTO && EOPNOTSUPP == 95
               && ECONNRESET == USBIP_LINUX_ECONNRESET,
               "newlib errno differs from Linux; add it to to_linux_errno()");

static int32_t to_linux_errno(int32_t status)
{
    if (status >= 0) {
        return status;
    }
    switch (-status) {
    case ENOENT: case EIO: case ENXIO: case EAGAIN: case ENOMEM:
    case EBUSY: case EXDEV: case ENODEV: case EINVAL: case ENOSPC:
    case EPIPE: case ENOSR: case ECOMM: case EPROTO: case EOPNOTSUPP:
    case ECONNRESET:
        return status;
    case EOVERFLOW:
        return -USBIP_LINUX_EOVERFLOW;
    case EILSEQ:
        return -USBIP_LINUX_EILSEQ;
    case EMSGSIZE:
        return -USBIP_LINUX_EMSGSIZE;
    case ESHUTDOWN:
        return -USBIP_LINUX_ESHUTDOWN;
    case ETIMEDOUT:
        return -USBIP_LINUX_ETIMEDOUT;
    case EINPROGRESS:
        return -USBIP_LINUX_EINPROGRESS;
    default:
        return -EIO;
    }
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

/* URB data is held in segments of URB_SEG_SIZE bytes, so a large bulk URB
   (Linux usb-storage sends up to 120 KiB) needs no large contiguous
   allocation, which a fragmented heap often cannot provide.  A bulk URB
   runs as one host transfer per segment; URB_SEG_SIZE is a multiple of
   every bulk max packet size, so the packets on the bus are the same as
   for a single transfer. */
#define URB_SEG_SIZE \
    ((CONFIG_USBIP_MAX_TRANSFER >= 1024) ? (CONFIG_USBIP_MAX_TRANSFER & ~1023) : CONFIG_USBIP_MAX_TRANSFER)
#define URB_MAX_SEGS ((CONFIG_USBIP_MAX_URB_SIZE + URB_SEG_SIZE - 1) / URB_SEG_SIZE)

typedef struct {
    uint8_t *seg[URB_MAX_SEGS];
    size_t len;
} urb_buf_t;

static size_t urb_buf_nsegs(const urb_buf_t *buf)
{
    return (buf->len + URB_SEG_SIZE - 1) / URB_SEG_SIZE;
}

static size_t urb_buf_seg_len(const urb_buf_t *buf, size_t i)
{
    const size_t rest = buf->len - i * URB_SEG_SIZE;
    return (rest < URB_SEG_SIZE) ? rest : URB_SEG_SIZE;
}

static void urb_buf_free(urb_buf_t *buf)
{
    for (size_t i = 0; i < URB_MAX_SEGS; i++) {
        free(buf->seg[i]);
        buf->seg[i] = NULL;
    }
    buf->len = 0;
}

static bool urb_buf_alloc(urb_buf_t *buf, size_t len)
{
    memset(buf, 0, sizeof(*buf));
    buf->len = len;
    for (size_t i = 0; i < urb_buf_nsegs(buf); i++) {
        buf->seg[i] = malloc(urb_buf_seg_len(buf, i));
        if (buf->seg[i] == NULL) {
            urb_buf_free(buf);
            return false;
        }
    }
    return true;
}

static bool send_ret_submit(int fd,
                            const usbip_header_t *request,
                            int32_t status,
                            const urb_buf_t *payload,
                            uint32_t payload_len)
{
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));

    reply.base.command = htonl(USBIP_RET_SUBMIT);
    reply.base.seqnum = request->base.seqnum;
    reply.base.devid = request->base.devid;
    reply.base.direction = request->base.direction;
    reply.base.ep = request->base.ep;

    reply.u.ret_submit.status = htonl((uint32_t)to_linux_errno(status));
    reply.u.ret_submit.actual_length = htonl(payload_len);
    reply.u.ret_submit.start_frame = htonl(0);
    reply.u.ret_submit.number_of_packets = htonl(0);
    reply.u.ret_submit.error_count = htonl(0);
    reply.u.ret_submit.padding = 0;

    if (payload_len == 0 || payload == NULL) {
        return write_all(fd, &reply, sizeof(reply));
    }

    /* MSG_MORE lets the stack send the header in the same TCP segment as
       the start of the payload, so the client reads both without waiting
       for a second segment.  The payload is written from its segments
       rather than copied next to the header. */
    if (!write_all_flags(fd, &reply, sizeof(reply), MSG_MORE)) {
        return false;
    }
    size_t left = payload_len;
    for (size_t i = 0; i < urb_buf_nsegs(payload) && left > 0; i++) {
        size_t n = urb_buf_seg_len(payload, i);
        if (n > left) {
            n = left;
        }
        left -= n;
        if (!write_all_flags(fd, payload->seg[i], n, left > 0 ? MSG_MORE : 0)) {
            return false;
        }
    }
    return true;
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

    reply.u.ret_submit.status = htonl((uint32_t)to_linux_errno(status));
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

    reply.u.ret_unlink.status = htonl((uint32_t)to_linux_errno(status));

    return write_all(fd, &reply, sizeof(reply));
}


/* Maximum URBs in flight per TCP connection.  It bounds the memory a client
   can tie up (each URB holds its data), not the USB side: no task blocks on
   a transfer, and pending reads wait at the backend.  Linux keeps up to 33
   URBs pending on an idle cdc-acm port (16 reads, 16 writes and the
   notification read). */
#define URB_STREAM_MAX_INFLIGHT CONFIG_USBIP_MAX_INFLIGHT_URBS

struct urb_work_item;

/* Per-connection context shared between the reader loop, the backend's
   completion callback and the writer task.  write_mutex guards both socket
   writes and in_flight[]: the writer sends a reply and frees its slot under
   the lock, so CMD_UNLINK either finds the URB still pending or knows its
   RET_SUBMIT has already gone out. */
typedef struct {
    int fd;
    volatile bool cancel;                   /* set when the client disconnects */
    SemaphoreHandle_t write_mutex;          /* serialises socket writes */
    SemaphoreHandle_t slot_sem;             /* counting: free in_flight[] entries */
    QueueHandle_t done_queue;               /* answered URBs, for the writer task */
    volatile bool writer_exited;
    atomic_int inflight_count;              /* URBs not yet answered to the client */
    struct {
        struct urb_work_item *item;         /* NULL = slot free */
        uint32_t seqnum;
    } in_flight[URB_STREAM_MAX_INFLIGHT];
} urb_stream_ctx_t;

/* One URB.  The reader loop builds it and submits it to the backend (or
   runs it inline for a virtual device).  The backend answers from its own
   task through urb_backend_done(), which queues the URB for the writer
   task; the writer sends the reply and frees everything.  A pending read
   therefore costs only this item and its buffer, as a pending URB does on
   a Linux host. */
typedef struct urb_work_item {
    urb_stream_ctx_t *stream;
    usbip_header_t request;
    usb_backend_req_t req;                  /* busid, endpoint, buffers; req.cancel = unlinked or stream closed */
    bool unlinked;                          /* CMD_UNLINK accepted for this URB */
    usbip_header_t unlink_request;          /* the CMD_UNLINK, to answer on completion */
    urb_buf_t out;                          /* OUT payload */
    urb_buf_t in;                           /* IN buffer */
    size_t seg;                             /* segments done (bulk URBs over one segment) */
    size_t done_len;                        /* IN bytes received over all segments */
    int status;                             /* result for the reply */
    size_t in_len;
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

/* RET_SUBMIT from the reader loop, serialised against the writer. */
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
        usb_backend_cancel(&item->req);
        ESP_LOGD(TAG, "CMD_UNLINK seq=%"PRIu32": cancelling", unlink_seq);
    } else {
        ok = send_ret_unlink(ctx->fd, request, 0);
        ESP_LOGD(TAG, "CMD_UNLINK seq=%"PRIu32": already completed", unlink_seq);
    }
    xSemaphoreGive(ctx->write_mutex);
    return ok;
}

static bool urb_is_in(const urb_work_item_t *item)
{
    return ntohl(item->request.base.direction) == USBIP_DIR_IN;
}

/* Hand an answered URB to the writer task. */
static void urb_queue_done(urb_work_item_t *item)
{
    if (xQueueSend(item->stream->done_queue, &item, 0) != pdTRUE) {
        /* Cannot happen: the queue holds as many entries as in_flight[]. */
        ESP_LOGE(TAG, "URB stream: done queue full, URB seq=%"PRIu32" lost",
                 ntohl(item->request.base.seqnum));
    }
}

/* Submit the URB's next segment (segment 0 is the whole URB when it fits
   one transfer).  A bulk URB larger than one segment runs as one host
   transfer per segment, in order on the same endpoint. */
static void urb_submit_segment(urb_work_item_t *item)
{
    const bool is_in = urb_is_in(item);
    urb_buf_t *buf = is_in ? &item->in : &item->out;
    const size_t n = (item->seg < urb_buf_nsegs(buf)) ? urb_buf_seg_len(buf, item->seg) : 0;
    uint8_t *data = (n > 0) ? buf->seg[item->seg] : NULL;

    item->req.out_data = is_in ? NULL : data;
    item->req.out_len = is_in ? 0 : n;
    item->req.in_data = is_in ? data : NULL;
    item->req.in_capacity = is_in ? n : 0;
    usb_backend_submit(&item->req);
}

/* Backend completion, from the backend task: run the next segment of a
   large bulk URB, or queue the URB for the writer.  A short IN segment
   ends the URB, as a short packet would. */
static void urb_backend_done(usb_backend_req_t *req, int status, size_t in_len)
{
    urb_work_item_t *item = (urb_work_item_t *)((char *)req - offsetof(urb_work_item_t, req));
    const bool is_in = urb_is_in(item);
    urb_buf_t *buf = is_in ? &item->in : &item->out;
    const size_t nsegs = urb_buf_nsegs(buf);

    if (status == 0) {
        const size_t n = (item->seg < nsegs) ? urb_buf_seg_len(buf, item->seg) : 0;
        item->done_len += is_in ? in_len : n;
        item->seg++;
        if (item->seg < nsegs && !(is_in && in_len < n)) {
            if (req->cancel) {
                status = -ECONNRESET;
            } else {
                urb_submit_segment(item);
                return;
            }
        }
    }
    item->status = status;
    item->in_len = (is_in && status == 0) ? item->done_len : 0;
    urb_queue_done(item);
}

/* Run a virtual device's URB inline: they complete at once. */
static void urb_run_virtual(urb_work_item_t *item, virtual_device_t *vdev)
{
    const uint32_t direction = ntohl(item->request.base.direction);
    const uint32_t endpoint = ntohl(item->request.base.ep);
    size_t in_len = 0;
    int status;

    if (endpoint == 0) {
        usb_setup_packet_t setup;
        memcpy(&setup, item->request.u.cmd_submit.setup, sizeof(setup));
        status = vdev->ops->control_transfer(vdev, &setup,
                                              item->out.seg[0], item->out.len,
                                              item->in.seg[0], item->in.len,
                                              &in_len);
    } else {
        const uint8_t ep_addr = (uint8_t)(endpoint |
            (direction == USBIP_DIR_IN ? 0x80 : 0x00));
        status = vdev->ops->data_transfer(vdev, ep_addr,
                                           item->out.seg[0], item->out.len,
                                           item->in.seg[0], item->in.len,
                                           &in_len);
    }
    item->status = status;
    item->in_len = (status == 0) ? in_len : 0;
}

/* Start a URB: inline for a virtual device, otherwise hand it to the
   backend, which answers through urb_backend_done(). */
static void urb_start(urb_work_item_t *item, virtual_device_t *vdev)
{
    const uint32_t direction = ntohl(item->request.base.direction);
    const uint32_t endpoint = ntohl(item->request.base.ep);
    const bool segmented = item->out.len > URB_SEG_SIZE || item->in.len > URB_SEG_SIZE;

    if (segmented && (vdev != NULL || endpoint == 0
                      || usb_backend_is_interrupt_endpoint(item->req.busid, endpoint,
                                                           direction == USBIP_DIR_IN))) {
        /* Only bulk URBs can be split into several transfers. */
        item->status = -EMSGSIZE;
        item->in_len = 0;
        urb_queue_done(item);
        return;
    }
    if (vdev != NULL) {
        urb_run_virtual(item, vdev);
        urb_queue_done(item);
        return;
    }

    item->req.endpoint_addr = (endpoint == 0) ? 0
        : (uint8_t)(endpoint | (direction == USBIP_DIR_IN ? 0x80 : 0x00));
    if (endpoint == 0) {
        memcpy(&item->req.setup, item->request.u.cmd_submit.setup, sizeof(item->req.setup));
    }
    item->req.done = urb_backend_done;
    urb_submit_segment(item);
}

/* Send a URB's reply.  Caller holds write_mutex. */
static void urb_send_reply(urb_work_item_t *item)
{
    urb_stream_ctx_t *ctx = item->stream;
    bool ok;

    if (ctx->fd < 0) {
        return;     /* the client is gone */
    }
    if (item->unlinked) {
        ok = send_ret_unlink(ctx->fd, &item->unlink_request, -ECONNRESET);
    } else if (urb_is_in(item)) {
        ok = send_ret_submit(ctx->fd, &item->request, item->status,
                             item->status == 0 ? &item->in : NULL,
                             item->status == 0 ? (uint32_t)item->in_len : 0);
    } else {
        /* OUT: actual_length is the number of bytes sent, with no payload.
           Clients check it (picotool treats a short PICOBOOT command write
           as a failure); the backend sends all or fails. */
        ok = send_ret_submit_out(ctx->fd, &item->request, item->status,
                                 item->status == 0 ? (uint32_t)item->out.len : 0);
    }
    if (item->status != 0 && !item->unlinked) {
        ESP_LOGD(TAG, "URB seq=%"PRIu32" ep=%"PRIu32" dir=%"PRIu32" status=%d",
                 ntohl(item->request.base.seqnum), ntohl(item->request.base.ep),
                 ntohl(item->request.base.direction), item->status);
    }
    if (!ok) {
        ctx->cancel = true;
    }
}

/* Per-connection writer: sends replies as URBs are answered, in completion
   order.  A NULL item stops it. */
static void urb_writer_task(void *arg)
{
    urb_stream_ctx_t *ctx = (urb_stream_ctx_t *)arg;
    urb_work_item_t *item;

    while (xQueueReceive(ctx->done_queue, &item, portMAX_DELAY) == pdTRUE) {
        if (item == NULL) {
            break;
        }
        /* Reply and release the slot under write_mutex, so a concurrent
           CMD_UNLINK sees either a pending URB or one already answered. */
        xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
        urb_send_reply(item);
        urb_stream_free_slot(ctx, item->slot);
        xSemaphoreGive(ctx->write_mutex);
        xSemaphoreGive(ctx->slot_sem);

        urb_buf_free(&item->out);
        urb_buf_free(&item->in);
        free(item);
    }
    ctx->writer_exited = true;
    vTaskDelete(NULL);
}

static void urb_stream_ctx_free(urb_stream_ctx_t *ctx)
{
    if (ctx->done_queue != NULL) {
        vQueueDelete(ctx->done_queue);
    }
    if (ctx->slot_sem != NULL) {
        vSemaphoreDelete(ctx->slot_sem);
    }
    if (ctx->write_mutex != NULL) {
        vSemaphoreDelete(ctx->write_mutex);
    }
    free(ctx);
}

static bool handle_urb_stream(int fd, const char imported_busid[32],
                               uint32_t expected_devid)
{
    /* Heap-allocate the stream context: the writer task and the backend's
       callbacks use it after this function starts unwinding. */
    urb_stream_ctx_t *ctx = calloc(1, sizeof(urb_stream_ctx_t));
    if (ctx == NULL) {
        return false;
    }
    ctx->fd = fd;
    ctx->write_mutex = xSemaphoreCreateMutex();
    ctx->slot_sem = xSemaphoreCreateCounting(URB_STREAM_MAX_INFLIGHT, URB_STREAM_MAX_INFLIGHT);
    ctx->done_queue = xQueueCreate(URB_STREAM_MAX_INFLIGHT + 1, sizeof(urb_work_item_t *));
    if (ctx->write_mutex == NULL || ctx->slot_sem == NULL || ctx->done_queue == NULL) {
        urb_stream_ctx_free(ctx);
        return false;
    }
    if (xTaskCreate(urb_writer_task, "urb_wr",
                    CONFIG_USBIP_SERVER_TASK_STACK, ctx,
                    CONFIG_USBIP_SERVER_TASK_PRIORITY, NULL) != pdPASS) {
        urb_stream_ctx_free(ctx);
        return false;
    }

    virtual_device_t *vdev = virtual_device_find_by_busid(imported_busid);

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
               loop (preserves TCP ordering), then hand off to the
               backend. --- */

        const uint32_t seqnum = ntohl(request.base.seqnum);
        const uint32_t devid = ntohl(request.base.devid);
        const uint32_t direction = ntohl(request.base.direction);
        const uint32_t endpoint = ntohl(request.base.ep);
        const int32_t req_len =
            (int32_t)ntohl((uint32_t)request.u.cmd_submit.transfer_buffer_length);
        const uint32_t packet_count =
            (uint32_t)ntohl((uint32_t)request.u.cmd_submit.number_of_packets);

        /* Quick validation — errors that don't need the backend. */
        if (req_len < 0) {
            ESP_LOGD(TAG, "  -> EINVAL (req_len < 0)");
            if (!stream_send_ret_submit(ctx, &request, -EINVAL))
                goto cleanup;
            continue;
        }
        if (req_len > CONFIG_USBIP_MAX_URB_SIZE) {
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

        /* Build the work item; it holds the URB's data segments. */
        urb_work_item_t *item = calloc(1, sizeof(urb_work_item_t));
        if (item == NULL) {
            if (direction == USBIP_DIR_OUT && req_len > 0) {
                if (!discard_exact(fd, (size_t)req_len)) goto cleanup;
            }
            if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                goto cleanup;
            continue;
        }

        /* Read OUT data payload (follows the header in the TCP stream). */
        const size_t out_len = (direction == USBIP_DIR_OUT) ? (size_t)req_len : 0;
        if (out_len > 0) {
            if (!urb_buf_alloc(&item->out, out_len)) {
                free(item);
                if (!discard_exact(fd, out_len)) goto cleanup;
                if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                    goto cleanup;
                continue;
            }
            bool read_ok = true;
            for (size_t i = 0; i < urb_buf_nsegs(&item->out) && read_ok; i++) {
                read_ok = read_exact(fd, item->out.seg[i], urb_buf_seg_len(&item->out, i));
            }
            if (!read_ok) {
                urb_buf_free(&item->out);
                free(item);
                goto cleanup;
            }
        }

        /* Allocate IN buffer. */
        const size_t in_capacity =
            (direction == USBIP_DIR_IN) ? (size_t)req_len : 0;
        if (in_capacity > 0 && !urb_buf_alloc(&item->in, in_capacity)) {
            urb_buf_free(&item->out);
            free(item);
            if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                goto cleanup;
            continue;
        }

        item->stream = ctx;
        item->request = request;
        memcpy(item->req.busid, imported_busid, sizeof(item->req.busid));

        /* Wait for an in-flight slot: the writer frees one per answered
           URB.  Only a client with more URBs pending than the limit ever
           waits here. */
        while (xSemaphoreTake(ctx->slot_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (ctx->cancel) {
                urb_buf_free(&item->out);
                urb_buf_free(&item->in);
                free(item);
                goto cleanup;
            }
        }
        item->slot = urb_stream_alloc_slot(ctx, seqnum, item);
        if (item->slot < 0) {
            ESP_LOGE(TAG, "URB stream: no in-flight slot (accounting error)");
            xSemaphoreGive(ctx->slot_sem);
            urb_buf_free(&item->out);
            urb_buf_free(&item->in);
            free(item);
            if (!stream_send_ret_submit(ctx, &request, -ENOMEM))
                goto cleanup;
            continue;
        }

        urb_start(item, vdev);
    }

cleanup:
    /* Cancel every pending URB; the backend answers each one (within
       about a second even for a transfer the device never finishes) and
       the writer frees them. */
    ctx->cancel = true;
    xSemaphoreTake(ctx->write_mutex, portMAX_DELAY);
    ctx->fd = -1;   /* the caller closes the socket; late replies must not reach a reused fd */
    for (int i = 0; i < URB_STREAM_MAX_INFLIGHT; i++) {
        if (ctx->in_flight[i].item != NULL) {
            usb_backend_cancel(&ctx->in_flight[i].item->req);
        }
    }
    xSemaphoreGive(ctx->write_mutex);
    for (int timeout = 0; timeout < 50; timeout++) {
        if (atomic_load(&ctx->inflight_count) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* Free the device's host channels for other sessions (a no-op for
       virtual devices, and deferred while a transfer is still in flight). */
    usb_backend_session_ended(imported_busid);
    if (atomic_load(&ctx->inflight_count) != 0) {
        /* The backend still owns a URB and will queue it for the writer;
           leak the context (and its writer) rather than free it under them. */
        ESP_LOGW(TAG, "URB stream: %d URB(s) still pending, leaking context",
                 atomic_load(&ctx->inflight_count));
        return false;
    }
    /* Stop the writer, then free everything. */
    urb_work_item_t *stop = NULL;
    xQueueSend(ctx->done_queue, &stop, portMAX_DELAY);
    for (int timeout = 0; timeout < 100 && !ctx->writer_exited; timeout++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!ctx->writer_exited) {
        ESP_LOGW(TAG, "URB stream: writer did not stop, leaking context");
        return false;
    }
    urb_stream_ctx_free(ctx);
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
