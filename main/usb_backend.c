#include "usb_backend.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/usb_host_hub.h"
#include "usb/usb_types_stack.h"

#include "virtual_device.h"

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
#include "driver/gpio.h"
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
#include "harness_io_expander.h"
#endif

#define USB_BACKEND_EVENT_QUEUE_LEN 16
#define USB_BACKEND_TASK_STACK 8192
#define USB_BACKEND_TASK_PRIORITY 9
#define USB_BACKEND_DAEMON_TASK_STACK 4096
#define USB_BACKEND_DAEMON_TASK_PRIORITY 10

#define USB_BACKEND_NUM_PIPES CONFIG_USBIP_NUM_PIPES

/* Software deadline for a transfer (the host library ignores timeout_ms). */
#define USB_BACKEND_XFER_TIMEOUT_MS 5000
/* How long a cancelled control transfer may take to come back before its
   slot is orphaned instead (it normally takes a few ms). */
#define USB_BACKEND_CANCEL_WAIT_MS 1000
/* Control transfers retired by another transfer's EP0 flush are submitted
   again, at most this many times. */
#define USB_BACKEND_MAX_CTRL_RESUBMITS 3

#ifndef USB_CLASS_HUB
#define USB_CLASS_HUB 0x09
#endif

typedef enum {
    USB_BACKEND_EVENT_NEW_DEV = 1,
    USB_BACKEND_EVENT_DEV_GONE = 2,
    USB_BACKEND_EVENT_RELEASE = 3,
} usb_backend_event_type_t;

typedef struct {
    usb_backend_event_type_t type;
    union {
        uint8_t address;
        usb_device_handle_t dev_hdl;
        char busid[32];
    } u;
} usb_backend_event_t;

/* Unified per-pipe request slot.  One slot per host pipe, allocated
   dynamically per URB from a shared pool.  The caller takes a counting
   semaphore to reserve capacity, fills a free slot, sets active = true,
   notifies the backend task, then waits on done_sem.  After completion
   the slot is freed back to the pool.

   Transfers are submitted non-blocking: the backend task submits all
   active pipes to the DWC hardware concurrently, then polls for
   completion or timeout. */
typedef struct {
    bool assigned;                  /* true = reserved for a device+endpoint */
    volatile bool active;           /* true = caller queued a transfer */
    volatile bool submitted;        /* true = transfer handed to DWC hw */
    volatile bool completed;        /* true = DWC callback has fired */
    volatile bool aborted;          /* true = we forced abort (timeout/cancel) */
    bool cancel_sent;               /* usb_host_transfer_cancel_control() called */
    TickType_t cancel_tick;         /* when it was called */
    uint8_t resubmits;              /* resubmissions after a collateral EP0 flush */
    bool orphaned;                  /* caller answered, transfer still in flight (state_mutex) */
    bool caller_left;               /* caller returned while orphaned (state_mutex) */

    char busid[32];
    uint8_t endpoint_addr;          /* 0 for control, 0x8N for IN, 0x0N for OUT */
    usb_setup_packet_t setup;       /* only used when endpoint_addr == 0 */
    const uint8_t *out_data;
    size_t out_len;
    uint8_t *in_data;
    size_t in_capacity;
    size_t *in_len_out;
    int *status_out;
    volatile bool *cancel;
    SemaphoreHandle_t done_sem;     /* pre-allocated, not per-call */

    /* In-flight transfer tracking (for non-blocking operation) */
    usb_transfer_t *xfer;           /* allocated transfer, NULL when idle */
    usb_device_handle_t dev_hdl;    /* cached device handle for abort */
    TickType_t deadline;            /* when the software timeout expires (set when queued) */
    bool is_control;                /* true = EP0 control transfer */
    bool is_in;                     /* true = device-to-host */
    size_t payload_len;             /* requested data length */
} usb_backend_pipe_req_t;

typedef struct {
    bool in_use;
    bool interfaces_claimed;
    uint32_t halted_eps;            /* endpoints halted by a STALL (see ep_halt_bit()) */
    usb_device_handle_t dev_hdl;
    usbip_backend_device_t device;
} usb_backend_device_slot_t;


typedef struct {
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t pipe_avail_sem;           /* counting sem: tracks free pipe slots */
    QueueHandle_t event_queue;
    TaskHandle_t task_hdl;

    usb_backend_pipe_req_t pipes[USB_BACKEND_NUM_PIPES];

    usb_host_client_handle_t client_hdl;
    usb_backend_device_slot_t devices[CONFIG_USBIP_MAX_DEVICES];
} usb_backend_state_t;

static const char *TAG = "usb_backend";
static usb_backend_state_t s_state;

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
static esp_err_t usb_backend_s3_usb_otg_devkit_power_init(void)
{
    const uint64_t pin_mask = (1ULL << CONFIG_USBIP_S3_USB_BOOST_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO) |
                              (1ULL << CONFIG_USBIP_S3_USB_SEL_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    gpio_set_level(CONFIG_USBIP_S3_USB_BOOST_EN_GPIO, 0);
    gpio_set_level(CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO, 1);
    gpio_set_level(CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO, 1);
    gpio_set_level(CONFIG_USBIP_S3_USB_SEL_GPIO, 1);

    ESP_LOGD(TAG,
             "Configured ESP32-S3-USB-OTG host power GPIOs (%d,%d,%d,%d)",
             CONFIG_USBIP_S3_USB_BOOST_EN_GPIO,
             CONFIG_USBIP_S3_USB_DEV_VBUS_EN_GPIO,
             CONFIG_USBIP_S3_USB_LIMIT_EN_GPIO,
             CONFIG_USBIP_S3_USB_SEL_GPIO);
    return ESP_OK;
}
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
static esp_err_t usb_backend_p4hil_usb_power_init(void)
{
    int exp_idx = CONFIG_USBIP_P4HIL_USB_POWER_EXPANDER_IDX;
    int pin     = CONFIG_USBIP_P4HIL_USB_POWER_EXPANDER_PIN;

    if (!harness_io_expander_is_initialized()) {
        ESP_LOGE(TAG, "IO expander not initialised, cannot enable USB power");
        return ESP_ERR_INVALID_STATE;
    }

    /* Set the expander pin as output, high */
    esp_err_t err = harness_io_expander_set_dir(exp_idx, (uint8_t)pin, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Exp%d[%d] as output: %s",
                 exp_idx, pin, esp_err_to_name(err));
        return err;
    }

    err = harness_io_expander_write_pin(exp_idx, (uint8_t)pin, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Exp%d[%d] high: %s",
                 exp_idx, pin, esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG,
             "P4HIL USB host power enabled on Exp%d[%d]",
             exp_idx, pin);
    return ESP_OK;
}
#endif

static uint32_t usb_speed_to_usbip(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return 1;
    case USB_SPEED_FULL:
        return 2;
    case USB_SPEED_HIGH:
        return 3;
    default:
        return 0;
    }
}

static void parse_descriptors(const usb_config_desc_t *config_desc, usbip_backend_device_t *device)
{
    if (config_desc == NULL || device == NULL) {
        return;
    }

    const uint8_t *raw = (const uint8_t *)config_desc;
    const size_t total_len = config_desc->wTotalLength;

    uint8_t intf_count = 0;
    uint8_t ep_count = 0;
    for (size_t offset = 0; offset + 2 <= total_len;) {
        const uint8_t desc_len = raw[offset];
        const uint8_t desc_type = raw[offset + 1];

        if (desc_len < 2 || (offset + desc_len) > total_len) {
            break;
        }

        if (desc_type == USB_B_DESCRIPTOR_TYPE_INTERFACE && desc_len >= 9 && intf_count < USBIP_MAX_INTERFACES) {
            device->interfaces[intf_count].interface_class = raw[offset + 5];
            device->interfaces[intf_count].interface_subclass = raw[offset + 6];
            device->interfaces[intf_count].interface_protocol = raw[offset + 7];
            intf_count++;
        } else if (desc_type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && desc_len >= 7 && ep_count < USBIP_MAX_ENDPOINTS) {
            device->endpoints[ep_count].address = raw[offset + 2];
            device->endpoints[ep_count].attributes = raw[offset + 3];
            device->endpoints[ep_count].max_packet_size = raw[offset + 4] | (raw[offset + 5] << 8);
            device->endpoints[ep_count].interval = raw[offset + 6];
            ep_count++;
        }

        offset += desc_len;
    }

    device->num_interfaces = intf_count;
    device->num_endpoints = ep_count;
}

static bool is_hub_device(const usb_device_desc_t *dev_desc, const usbip_backend_device_t *device)
{
    if (dev_desc != NULL && dev_desc->bDeviceClass == USB_CLASS_HUB) {
        return true;
    }

    for (uint8_t i = 0; i < device->num_interfaces; i++) {
        if (device->interfaces[i].interface_class == USB_CLASS_HUB) {
            return true;
        }
    }

    return false;
}

/* Copy a UTF-16LE string descriptor into a printable ASCII string. */
static void str_desc_to_ascii(const usb_str_desc_t *desc, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (desc == NULL || desc->bLength < 2) {
        return;
    }
    size_t n_chars = (desc->bLength - 2) / 2;
    size_t o = 0;
    for (size_t i = 0; i < n_chars && o + 1 < out_size; i++) {
        uint16_t c = desc->wData[i];
        if (c == 0) {
            break;    /* some devices NUL-pad their strings */
        }
        out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[o] = '\0';
}

#define USB_BACKEND_MAX_HUB_DEPTH 6

/* Resolve the parent hub of a device and build its Linux style port path
   ("1-1" on the root port, "1-1.3" on port 3 of the hub on the root port).
   The root port is reported as port 1 of bus 1, like a Linux root hub.
   Walks parent handles up to the root port; hubs stay alive while they
   have children, so the handles are valid. */
static void resolve_topology(const usb_device_info_t *dev_info,
                             uint8_t *parent_hub_addr,
                             uint8_t *parent_port,
                             char *path, size_t path_size)
{
    uint8_t ports[USB_BACKEND_MAX_HUB_DEPTH];
    size_t depth = 0;
    *parent_hub_addr = 0;
    *parent_port = 0;

    usb_device_info_t info = *dev_info;
    while (info.parent.dev_hdl != NULL && depth < USB_BACKEND_MAX_HUB_DEPTH) {
        ports[depth++] = info.parent.port_num;
        usb_device_info_t parent_info;
        if (usb_host_device_info(info.parent.dev_hdl, &parent_info) != ESP_OK) {
            /* Parent unknown: fall back to an address busid */
            snprintf(path, path_size, "1-%u", dev_info->dev_addr);
            return;
        }
        if (depth == 1) {
            *parent_hub_addr = parent_info.dev_addr;
            *parent_port = info.parent.port_num;
        }
        info = parent_info;
    }

    size_t off = strlcpy(path, "1-1", path_size);
    while (depth > 0 && off < path_size) {
        off += snprintf(path + off, path_size - off, ".%u", ports[--depth]);
    }
}

static bool busid_matches_key(const char key[32], const char stored_busid[32])
{
    char expected[32] = {0};
    strlcpy(expected, stored_busid, sizeof(expected));
    return memcmp(key, expected, sizeof(expected)) == 0;
}

static int find_slot_by_busid_locked(const char busid[32])
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (busid_matches_key(busid, s_state.devices[i].device.busid)) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_handle_locked(usb_device_handle_t dev_hdl)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (s_state.devices[i].dev_hdl == dev_hdl) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_devnum_locked(uint32_t devnum)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }
        if (s_state.devices[i].device.devnum == devnum) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot_locked(void)
{
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_state.devices[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int alloc_pipe_locked(void)
{
    for (int i = 0; i < USB_BACKEND_NUM_PIPES; i++) {
        if (!s_state.pipes[i].assigned) {
            s_state.pipes[i].assigned = true;
            s_state.pipes[i].active = false;
            return i;
        }
    }
    return -1;
}

static void clear_slot_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES) {
        return;
    }

    /* Pipe slots are allocated dynamically per URB and freed by the
       caller — no per-device pipe state to clean up here. */

    memset(&s_state.devices[slot], 0, sizeof(s_state.devices[slot]));
}

static void release_interfaces_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return;
    }
    if (!s_state.devices[slot].interfaces_claimed) {
        return;
    }

    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    const usbip_backend_device_t *device = &s_state.devices[slot].device;

    for (uint8_t i = 0; i < device->num_interfaces; i++) {
        esp_err_t err = usb_host_interface_release(s_state.client_hdl, dev_hdl, i);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_interface_release(%u) failed: %s", i, esp_err_to_name(err));
        }
    }

    /* Pipe slots are allocated dynamically per URB — nothing to free here. */

    s_state.devices[slot].interfaces_claimed = false;
}

static esp_err_t ensure_interfaces_claimed_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.devices[slot].interfaces_claimed) {
        return ESP_OK;
    }

    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    const usbip_backend_device_t *device = &s_state.devices[slot].device;

    ESP_LOGD(TAG, "Claiming %u interfaces for %s (lazy)", device->num_interfaces, device->busid);
    for (uint8_t i = 0; i < device->num_interfaces; i++) {
        esp_err_t err = usb_host_interface_claim(s_state.client_hdl, dev_hdl, i, 0);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_interface_claim(%u) failed: %s", i, esp_err_to_name(err));
            return err;
        }
    }

    /* Pipe slots are allocated dynamically per URB — no per-endpoint
       reservation needed. */

    s_state.devices[slot].interfaces_claimed = true;
    return ESP_OK;
}

static void close_slot_locked(int slot)
{
    if (slot < 0 || slot >= CONFIG_USBIP_MAX_DEVICES || !s_state.devices[slot].in_use) {
        return;
    }

    release_interfaces_locked(slot);

    usb_device_handle_t dev_hdl = s_state.devices[slot].dev_hdl;
    if (dev_hdl != NULL) {
        esp_err_t err = usb_host_device_close(s_state.client_hdl, dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_device_close failed: %s", esp_err_to_name(err));
        }
    }

    clear_slot_locked(slot);
}

static void usb_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    usb_backend_state_t *state = (usb_backend_state_t *)arg;

    usb_backend_event_t evt;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGW(TAG, "USB device attached at address %u", event_msg->new_dev.address);
        evt.type = USB_BACKEND_EVENT_NEW_DEV;
        evt.u.address = event_msg->new_dev.address;
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "USB device detached");
        evt.type = USB_BACKEND_EVENT_DEV_GONE;
        evt.u.dev_hdl = event_msg->dev_gone.dev_hdl;
    } else {
        return;
    }

    if (xQueueSend(state->event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Dropping USB event type=%d due to full queue", (int)evt.type);
    }
}

static void export_new_device(uint8_t address)
{
    usb_device_handle_t dev_hdl = NULL;
    esp_err_t err = usb_host_device_open(s_state.client_hdl, address, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb_host_device_open(%u) failed: %s", address, esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *dev_desc = NULL;
    err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if (err != ESP_OK || dev_desc == NULL) {
        ESP_LOGD(TAG, "usb_host_get_device_descriptor failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    usb_device_info_t dev_info;
    err = usb_host_device_info(dev_hdl, &dev_info);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "usb_host_device_info failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    usbip_backend_device_t device;
    memset(&device, 0, sizeof(device));

    device.present = true;
    device.busnum = 1;
    device.devnum = dev_info.dev_addr;
    device.dev_addr = dev_info.dev_addr;
    device.speed = usb_speed_to_usbip(dev_info.speed);
    str_desc_to_ascii(dev_info.str_desc_manufacturer, device.manufacturer, sizeof(device.manufacturer));
    str_desc_to_ascii(dev_info.str_desc_product, device.product, sizeof(device.product));
    str_desc_to_ascii(dev_info.str_desc_serial_num, device.serial, sizeof(device.serial));

    device.id_vendor = dev_desc->idVendor;
    device.id_product = dev_desc->idProduct;
    device.bcd_device = dev_desc->bcdDevice;
    device.device_class = dev_desc->bDeviceClass;
    device.device_subclass = dev_desc->bDeviceSubClass;
    device.device_protocol = dev_desc->bDeviceProtocol;
    device.num_configurations = dev_desc->bNumConfigurations;
    device.configuration_value = dev_info.bConfigurationValue;

    const usb_config_desc_t *config_desc = NULL;
    err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err == ESP_OK && config_desc != NULL) {
        parse_descriptors(config_desc, &device);
        device.max_power_ma = (uint16_t)config_desc->bMaxPower * 2;
    }

    if (is_hub_device(dev_desc, &device)) {
        ESP_LOGW(TAG, "Hub detected at address=%u, managing locally (not exported)", address);
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    char port_path[32];
    resolve_topology(&dev_info, &device.parent_hub_addr, &device.parent_port,
                     port_path, sizeof(port_path));

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    strlcpy(device.busid, port_path, sizeof(device.busid));
    snprintf(device.path, sizeof(device.path), "/esp-usb-host/%s", port_path);

    int existing_slot = find_slot_by_devnum_locked(device.devnum);
    if (existing_slot >= 0) {
        close_slot_locked(existing_slot);
    }
    existing_slot = find_slot_by_busid_locked(device.busid);
    if (existing_slot >= 0) {
        close_slot_locked(existing_slot);
    }

    const int free_slot = find_free_slot_locked();
    if (free_slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        ESP_LOGD(TAG, "No free export slots left (max=%d)", CONFIG_USBIP_MAX_DEVICES);
        usb_host_device_close(s_state.client_hdl, dev_hdl);
        return;
    }

    s_state.devices[free_slot].in_use = true;
    s_state.devices[free_slot].dev_hdl = dev_hdl;
    s_state.devices[free_slot].device = device;
    s_state.devices[free_slot].interfaces_claimed = false;

    /* Pipe slots are allocated dynamically per URB — no per-device
       pre-allocation needed. */

    xSemaphoreGive(s_state.state_mutex);

    ESP_LOGI(TAG,
             "Exporting USB device busid=%s vid=%04x pid=%04x (address %u)",
             device.busid,
             device.id_vendor,
             device.id_product,
             device.dev_addr);
}

static void remove_gone_device(usb_device_handle_t dev_hdl)
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    const int slot = find_slot_by_handle_locked(dev_hdl);
    if (slot >= 0) {
        char busid[32] = {0};
        strlcpy(busid, s_state.devices[slot].device.busid, sizeof(busid));
        close_slot_locked(slot);
        xSemaphoreGive(s_state.state_mutex);
        ESP_LOGW(TAG, "USB device disconnected: %s", busid);
        return;
    }

    xSemaphoreGive(s_state.state_mutex);
}

/* A client's USB/IP session for the device ended: release its interfaces,
   which frees their host pipes and DWC channels for other devices.  The host
   has 16 channels and every device's EP0 and each hub's interrupt endpoint
   hold one for good, so a few devices with many endpoints claimed (CDC + MSC
   + HID + MIDI) exhaust them and later claims fail.  The next session claims
   the interfaces again, with fresh pipes at DATA0, on its first non-control
   transfer.  A device with a transfer still in flight keeps its interfaces. */
static void release_session_device(const char busid[32])
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(busid);
    if (slot >= 0 && s_state.devices[slot].interfaces_claimed) {
        bool busy = false;
        for (int i = 0; i < USB_BACKEND_NUM_PIPES; i++) {
            if (s_state.pipes[i].submitted && busid_matches_key(busid, s_state.pipes[i].busid)) {
                busy = true;
                break;
            }
        }
        if (busy) {
            ESP_LOGD(TAG, "%s: session ended with a transfer in flight, interfaces kept", busid);
        } else {
            release_interfaces_locked(slot);
            s_state.devices[slot].halted_eps = 0;
            ESP_LOGI(TAG, "%s: session ended, interfaces released", busid);
        }
    }
    xSemaphoreGive(s_state.state_mutex);
}

static void process_backend_events(void)
{
    usb_backend_event_t evt;
    while (xQueueReceive(s_state.event_queue, &evt, 0) == pdTRUE) {
        if (evt.type == USB_BACKEND_EVENT_NEW_DEV) {
            export_new_device(evt.u.address);
        } else if (evt.type == USB_BACKEND_EVENT_DEV_GONE) {
            remove_gone_device(evt.u.dev_hdl);
        } else if (evt.type == USB_BACKEND_EVENT_RELEASE) {
            release_session_device(evt.u.busid);
        }
    }
}

void usb_backend_session_ended(const char busid[32])
{
    if (busid == NULL || s_state.event_queue == NULL) {
        return;
    }
    usb_backend_event_t evt = {
        .type = USB_BACKEND_EVENT_RELEASE,
    };
    strlcpy(evt.u.busid, busid, sizeof(evt.u.busid));
    if (xQueueSend(s_state.event_queue, &evt, 0) == pdTRUE) {
        xTaskNotifyGive(s_state.task_hdl);
    }
}

/* Completion callback for all transfer types.  The context pointer is
   the pipe slot, so we just set the completed flag.  The backend task
   polls this flag to detect completion. */
static void pipe_transfer_done_cb(usb_transfer_t *transfer)
{
    usb_backend_pipe_req_t *pipe = (usb_backend_pipe_req_t *)transfer->context;
    pipe->completed = true;
}

static int map_transfer_status_to_errno(usb_transfer_status_t status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        return 0;
    case USB_TRANSFER_STATUS_TIMED_OUT:
        return -ETIMEDOUT;
    case USB_TRANSFER_STATUS_CANCELED:
        return -ECONNRESET;
    case USB_TRANSFER_STATUS_STALL:
        return -EPIPE;
    case USB_TRANSFER_STATUS_NO_DEVICE:
        return -ENODEV;
    case USB_TRANSFER_STATUS_ERROR:
        return -EPROTO;
    case USB_TRANSFER_STATUS_OVERFLOW:
        return -EOVERFLOW;
    default:
        return -EIO;
    }
}

/* Bit for an endpoint in usb_backend_device_slot_t.halted_eps: OUT
   endpoints in bits 0-15, IN endpoints in bits 16-31. */
static uint32_t ep_halt_bit(uint8_t endpoint_addr)
{
    return 1u << ((endpoint_addr & 0x0f) + ((endpoint_addr & 0x80) ? 16 : 0));
}

/* Collect the endpoints of the claimed (alternate setting 0) interfaces,
   or of one interface when intf_filter >= 0. */
static int collect_claimed_endpoints_locked(int dev_slot, int intf_filter,
                                            uint8_t *out, int max)
{
    int count = 0;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_state.devices[dev_slot].dev_hdl, &cfg) != ESP_OK
        || cfg == NULL) {
        return 0;
    }
    for (int intf_num = 0; intf_num < s_state.devices[dev_slot].device.num_interfaces; intf_num++) {
        if (intf_filter >= 0 && intf_num != intf_filter) {
            continue;
        }
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, intf_num, 0, &offset);
        if (intf == NULL) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints && count < max; i++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
            if (ep != NULL) {
                out[count++] = ep->bEndpointAddress;
            }
        }
    }
    return count;
}

/* Reset a host endpoint after a request that reset the device's data
   toggle for it (USB 2.0 9.4.5): halt (cancels a transfer in flight),
   flush, start the toggle again at DATA0 and make the pipe active.
   Runs in the backend task, like the abort path. */
static void reset_host_endpoint(usb_device_handle_t dev_hdl, uint8_t endpoint_addr)
{
    usb_host_endpoint_halt(dev_hdl, endpoint_addr);
    usb_host_endpoint_flush(dev_hdl, endpoint_addr);
    esp_err_t err = usb_host_endpoint_reset_toggle(dev_hdl, endpoint_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset toggle failed: %s (ep=0x%02x)", esp_err_to_name(err), endpoint_addr);
    }
    usb_host_endpoint_clear(dev_hdl, endpoint_addr);
}

/* Linux usbip-host applies CLEAR_FEATURE(ENDPOINT_HALT), SET_INTERFACE and
   SET_CONFIGURATION to its own host controller as well (stub_rx.c
   tweak_*_cmd()).  Mirror that once the device has accepted the request:
   the device's toggles are back at DATA0, so ours must be too, and an
   endpoint halted by a STALL becomes usable again. */
static void apply_standard_request_to_host(const usb_backend_pipe_req_t *pipe)
{
    const usb_setup_packet_t *setup = &pipe->setup;
    const uint8_t std_out = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD;
    int intf_filter;

    if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT)
        && setup->bRequest == USB_B_REQUEST_CLEAR_FEATURE
        && setup->wValue == 0 /* ENDPOINT_HALT */) {
        intf_filter = -2;   /* the endpoint in wIndex */
    } else if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)
               && setup->bRequest == USB_B_REQUEST_SET_INTERFACE) {
        intf_filter = setup->wIndex & 0xff;
    } else if (setup->bmRequestType == (std_out | USB_BM_REQUEST_TYPE_RECIP_DEVICE)
               && setup->bRequest == USB_B_REQUEST_SET_CONFIGURATION) {
        intf_filter = -1;   /* every interface */
    } else {
        return;
    }

    uint8_t eps[USBIP_MAX_ENDPOINTS];
    int num_eps = 0;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    if (dev_slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        return;
    }
    usb_backend_device_slot_t *dev = &s_state.devices[dev_slot];
    if (intf_filter == -2) {
        const uint8_t ep = setup->wIndex & 0xff;
        if ((ep & 0x0f) != 0) {
            eps[num_eps++] = ep;
        }
    } else {
        if (intf_filter >= 0 && (setup->wValue & 0xff) != 0) {
            ESP_LOGW(TAG, "%s: SET_INTERFACE %d alt %d; only alt 0 endpoints are mapped",
                     pipe->busid, intf_filter, setup->wValue & 0xff);
        }
        num_eps = collect_claimed_endpoints_locked(dev_slot, intf_filter, eps, USBIP_MAX_ENDPOINTS);
    }
    for (int i = 0; i < num_eps; i++) {
        dev->halted_eps &= ~ep_halt_bit(eps[i]);
    }
    /* Pipes are allocated (at DATA0) when the interfaces are claimed, so
       there is nothing to reset before the first non-control transfer. */
    const bool claimed = dev->interfaces_claimed;
    const usb_device_handle_t dev_hdl = dev->dev_hdl;
    xSemaphoreGive(s_state.state_mutex);

    if (!claimed) {
        return;
    }
    for (int i = 0; i < num_eps; i++) {
        ESP_LOGD(TAG, "%s: reset host endpoint 0x%02x (bRequest %u)",
                 pipe->busid, eps[i], setup->bRequest);
        reset_host_endpoint(dev_hdl, eps[i]);
    }
}

/* After a non-control transfer failed: a STALL leaves the host pipe halted
   until the client sends CLEAR_FEATURE(ENDPOINT_HALT), and URBs for the
   endpoint fail with -EPIPE meanwhile (as on Linux).  Any other error
   halts the pipe too, but the device is not halted, so the pipe is made
   active again straight away (the toggle is still right). */
static void note_transfer_failure(const usb_backend_pipe_req_t *pipe, usb_transfer_status_t usb_status)
{
    if (usb_status == USB_TRANSFER_STATUS_STALL) {
        xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
        const int dev_slot = find_slot_by_busid_locked(pipe->busid);
        if (dev_slot >= 0) {
            s_state.devices[dev_slot].halted_eps |= ep_halt_bit(pipe->endpoint_addr);
        }
        xSemaphoreGive(s_state.state_mutex);
    } else if (usb_status == USB_TRANSFER_STATUS_ERROR
               || usb_status == USB_TRANSFER_STATUS_OVERFLOW) {
        usb_host_endpoint_clear(pipe->dev_hdl, pipe->endpoint_addr);
    }
}

static bool endpoint_is_halted(const usb_backend_pipe_req_t *pipe)
{
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    const bool halted = dev_slot >= 0
        && (s_state.devices[dev_slot].halted_eps & ep_halt_bit(pipe->endpoint_addr)) != 0;
    xSemaphoreGive(s_state.state_mutex);
    return halted;
}

static uint16_t get_endpoint_mps_locked(int dev_slot, uint8_t endpoint_addr)
{
    uint16_t mps = 64;  /* safe default */
    usb_device_handle_t dev = s_state.devices[dev_slot].dev_hdl;
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || cfg == NULL) {
        return mps;
    }
    for (int intf_num = 0; intf_num < s_state.devices[dev_slot].device.num_interfaces; intf_num++) {
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, intf_num, 0, &offset);
        if (intf == NULL) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &offset);
            if (ep != NULL && ep->bEndpointAddress == endpoint_addr) {
                return ep->wMaxPacketSize;
            }
        }
    }
    return mps;
}

/* Submit one transfer to the DWC hardware.  Returns 0 on success (transfer
   is now in-flight; the callback will set pipe->completed).  Returns a
   negative errno on immediate failure (bad device, no memory, etc.). */
static int prepare_and_submit_transfer(usb_backend_pipe_req_t *pipe)
{
    if (pipe->in_len_out != NULL) {
        *pipe->in_len_out = 0;
    }

    pipe->is_control = (pipe->endpoint_addr == 0);
    pipe->is_in = pipe->is_control
        ? (pipe->setup.bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0
        : (pipe->endpoint_addr & 0x80) != 0;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int dev_slot = find_slot_by_busid_locked(pipe->busid);
    if (dev_slot >= 0) {
        if (!pipe->is_control) {
            esp_err_t claim_err = ensure_interfaces_claimed_locked(dev_slot);
            if (claim_err != ESP_OK) {
                xSemaphoreGive(s_state.state_mutex);
                return -EIO;
            }
            if (s_state.devices[dev_slot].halted_eps & ep_halt_bit(pipe->endpoint_addr)) {
                xSemaphoreGive(s_state.state_mutex);
                return -EPIPE;
            }
        }
        pipe->dev_hdl = s_state.devices[dev_slot].dev_hdl;
    } else {
        pipe->dev_hdl = NULL;
    }

    pipe->payload_len = pipe->is_in ? pipe->in_capacity : pipe->out_len;

    /* For IN bulk/interrupt, round up to MPS while we hold the mutex. */
    size_t xfer_len = pipe->is_control
        ? (USB_SETUP_PACKET_SIZE + pipe->payload_len)
        : pipe->payload_len;
    if (!pipe->is_control && pipe->is_in && pipe->payload_len > 0 && dev_slot >= 0) {
        uint16_t mps = get_endpoint_mps_locked(dev_slot, pipe->endpoint_addr);
        if (mps > 0 && (xfer_len % mps) != 0) {
            xfer_len = ((xfer_len + mps - 1) / mps) * mps;
        }
    }
    xSemaphoreGive(s_state.state_mutex);

    if (pipe->dev_hdl == NULL) {
        return -ENODEV;
    }
    if (pipe->payload_len > CONFIG_USBIP_MAX_TRANSFER) {
        return -EMSGSIZE;
    }

    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(xfer_len, 0, &transfer);
    if (err != ESP_OK || transfer == NULL) {
        return -ENOMEM;
    }

    if (pipe->is_control) {
        memcpy(transfer->data_buffer, &pipe->setup, USB_SETUP_PACKET_SIZE);
        if (!pipe->is_in && pipe->out_len > 0 && pipe->out_data != NULL) {
            memcpy(transfer->data_buffer + USB_SETUP_PACKET_SIZE,
                   pipe->out_data, pipe->out_len);
        }
    } else if (!pipe->is_in && pipe->out_len > 0 && pipe->out_data != NULL) {
        memcpy(transfer->data_buffer, pipe->out_data, pipe->out_len);
    }

    transfer->callback = pipe_transfer_done_cb;
    transfer->context = pipe;
    transfer->device_handle = pipe->dev_hdl;
    transfer->bEndpointAddress = pipe->endpoint_addr;
    transfer->num_bytes = xfer_len;
    transfer->timeout_ms = USB_BACKEND_XFER_TIMEOUT_MS;

    if (pipe->is_control) {
        err = usb_host_transfer_submit_control(s_state.client_hdl, transfer);
    } else {
        err = usb_host_transfer_submit(transfer);
    }
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "submit failed: %s (ep=0x%02x)",
                 esp_err_to_name(err), pipe->endpoint_addr);
        usb_host_transfer_free(transfer);
        if (err != ESP_ERR_INVALID_STATE) {
            return -EIO;
        }
        /* A data pipe refuses URBs while it is halted (Linux answers
           -EPIPE).  The default pipe refuses them while the host library
           recovers it (after a STALL, an error or a cancel it stays halted
           until usbh_process() clears it) and once the device is gone; the
           device slot was found above, so retry until the deadline. */
        return pipe->is_control ? -EAGAIN : -EPIPE;
    }

    pipe->xfer = transfer;
    pipe->submitted = true;
    pipe->completed = false;
    pipe->aborted = false;
    pipe->cancel_sent = false;
    return 0;
}

/* Finish a transfer that has completed (callback fired) or was aborted.
   Reads the hardware result, copies IN data, frees the transfer object,
   and returns the errno to report to the caller. */
static int complete_transfer(usb_backend_pipe_req_t *pipe)
{
    usb_transfer_t *transfer = pipe->xfer;
    int status;

    if (pipe->completed) {
        status = map_transfer_status_to_errno(transfer->status);
    } else {
        /* Aborted without callback firing (should be rare). */
        status = -ETIMEDOUT;
    }

    if (status != 0) {
        ESP_LOGD(TAG, "transfer failed: usb_status=%d errno=%d (ep=0x%02x)",
                 transfer->status, status, pipe->endpoint_addr);
    }

    /* Copy IN data on success. */
    if (status == 0 && pipe->is_in && pipe->in_data != NULL
        && pipe->in_capacity > 0) {
        size_t bytes;
        if (pipe->is_control) {
            bytes = (transfer->actual_num_bytes > USB_SETUP_PACKET_SIZE)
                        ? (transfer->actual_num_bytes - USB_SETUP_PACKET_SIZE)
                        : 0;
        } else {
            bytes = transfer->actual_num_bytes;
            if (bytes > pipe->payload_len) {
                bytes = pipe->payload_len;
            }
        }
        const size_t copy_len =
            (bytes > pipe->in_capacity) ? pipe->in_capacity : bytes;
        const uint8_t *src = pipe->is_control
            ? (transfer->data_buffer + USB_SETUP_PACKET_SIZE)
            : transfer->data_buffer;
        memcpy(pipe->in_data, src, copy_len);
        if (pipe->in_len_out != NULL) {
            *pipe->in_len_out = copy_len;
        }
    } else if (pipe->in_len_out != NULL) {
        *pipe->in_len_out = 0;
    }

    usb_host_transfer_free(transfer);
    pipe->xfer = NULL;

    return status;
}

/* Answer the caller of an aborted transfer that is still in flight, and
   keep its slot (and the transfer) until the completion callback fires. */
static void orphan_transfer(usb_backend_pipe_req_t *pipe)
{
    const int status = (pipe->cancel != NULL && *pipe->cancel) ? -ECONNRESET : -ETIMEDOUT;
    ESP_LOGD(TAG, "%s ep 0x%02x: aborted transfer still in flight, holding its slot",
             pipe->busid, pipe->endpoint_addr);

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    pipe->orphaned = true;
    xSemaphoreGive(s_state.state_mutex);

    pipe->active = false;
    if (pipe->status_out != NULL) {
        *pipe->status_out = status;
    }
    /* Everything below belongs to the caller, which returns now. */
    pipe->status_out = NULL;
    pipe->in_len_out = NULL;
    pipe->in_data = NULL;
    pipe->out_data = NULL;
    pipe->cancel = NULL;
    xSemaphoreGive(pipe->done_sem);
}

/* The orphaned transfer has completed: free it and the slot.  If the caller
   has not looked at the slot yet, it releases the slot itself. */
static void release_orphan(usb_backend_pipe_req_t *pipe)
{
    usb_host_transfer_free(pipe->xfer);
    pipe->xfer = NULL;
    pipe->submitted = false;
    pipe->completed = false;
    pipe->aborted = false;

    bool give = false;
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    pipe->orphaned = false;
    if (pipe->caller_left) {
        pipe->caller_left = false;
        pipe->assigned = false;
        give = true;
    }
    xSemaphoreGive(s_state.state_mutex);
    if (give) {
        xSemaphoreGive(s_state.pipe_avail_sem);
    }
}

static void usb_backend_daemon_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_backend_task(void *arg)
{
    (void)arg;

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = USB_BACKEND_EVENT_QUEUE_LEN,
        .async = {
            .client_event_callback = usb_client_event_cb,
            .callback_arg = &s_state,
        },
    };

    esp_err_t err = usb_host_client_register(&client_config, &s_state.client_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(TAG, "USB backend task running (%d pipe slots)", USB_BACKEND_NUM_PIPES);

    while (true) {
        /* Wait for a notification from a caller, or wake every 10ms to
           poll timeouts. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        /* Pump USB events — this also delivers transfer completion
           callbacks (pipe->completed = true). */
        err = usb_host_client_handle_events(s_state.client_hdl, 0);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "usb_host_client_handle_events failed: %s", esp_err_to_name(err));
        }

        process_backend_events();

        const TickType_t now = xTaskGetTickCount();

        /* Phase 1: Submit any newly-active pipes to the DWC hardware.
           These transfers now run concurrently in hardware. */
        for (int i = 0; i < USB_BACKEND_NUM_PIPES; i++) {
            usb_backend_pipe_req_t *pipe = &s_state.pipes[i];
            if (!pipe->active || pipe->submitted) {
                continue;
            }

            int ret = prepare_and_submit_transfer(pipe);
            if (ret == -EAGAIN) {
                /* EP0 is being recovered: try again on the next pass. */
                if (pipe->cancel != NULL && *pipe->cancel) {
                    ret = -ECONNRESET;
                } else if (now >= pipe->deadline) {
                    ret = -ETIMEDOUT;
                } else {
                    continue;
                }
            }
            if (ret < 0) {
                /* Immediate failure (bad device, no memory, etc.) —
                   complete the request now. */
                pipe->active = false;
                if (pipe->status_out != NULL) {
                    *pipe->status_out = ret;
                }
                xSemaphoreGive(pipe->done_sem);
            }
        }

        /* Phase 2: Check all in-flight transfers for completion,
           timeout, or cancellation. */
        for (int i = 0; i < USB_BACKEND_NUM_PIPES; i++) {
            usb_backend_pipe_req_t *pipe = &s_state.pipes[i];
            if (!pipe->submitted) {
                continue;
            }
            if (pipe->orphaned) {
                if (pipe->completed) {
                    release_orphan(pipe);
                }
                continue;
            }

            /* Detect external cancel requests. */
            if (!pipe->aborted && pipe->cancel != NULL && *pipe->cancel) {
                pipe->aborted = true;
                ESP_LOGD(TAG, "transfer cancelled (ep=0x%02x)", pipe->endpoint_addr);
            }

            /* Detect software timeout (DWC hardware doesn't). */
            if (!pipe->aborted && now >= pipe->deadline) {
                pipe->aborted = true;
                ESP_LOGD(TAG, "transfer timed out (ep=0x%02x)", pipe->endpoint_addr);
            }

            /* If we need to abort a non-control transfer that hasn't
               completed yet, halt+flush the endpoint to force the DWC
               hardware to fire the completion callback. */
            if (pipe->aborted && !pipe->completed && !pipe->is_control
                && pipe->dev_hdl != NULL) {
                usb_host_endpoint_halt(pipe->dev_hdl, pipe->endpoint_addr);
                usb_host_endpoint_flush(pipe->dev_hdl, pipe->endpoint_addr);
                /* Pump events so the flush/halt callback fires. */
                for (int j = 0; j < 50 && !pipe->completed; j++) {
                    usb_host_client_handle_events(s_state.client_hdl,
                                                  pdMS_TO_TICKS(10));
                }
                usb_host_endpoint_clear(pipe->dev_hdl, pipe->endpoint_addr);
            }

            /* Cancel an aborted control transfer: the host library retires
               it (and, if it was on the bus, every control transfer queued
               to the device; see the resubmission below) and calls back
               with USB_TRANSFER_STATUS_CANCELED.  Without this, a transfer
               the device never finishes blocks the device's EP0 for good:
               every later control transfer queues behind it.  Wait for the
               callback on later passes. */
            if (pipe->aborted && !pipe->completed && pipe->is_control
                && pipe->dev_hdl != NULL) {
                if (!pipe->cancel_sent) {
                    pipe->cancel_sent = true;
                    pipe->cancel_tick = now;
                    esp_err_t cerr = usb_host_transfer_cancel_control(s_state.client_hdl, pipe->xfer);
                    if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_STATE) {
                        ESP_LOGW(TAG, "%s: control cancel failed: %s", pipe->busid, esp_err_to_name(cerr));
                        pipe->cancel_tick = now - pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS);
                    } else {
                        ESP_LOGI(TAG, "%s: control transfer %s, cancelled (bmRequestType 0x%02x bRequest 0x%02x wValue 0x%04x wIndex 0x%04x)",
                                 pipe->busid,
                                 (pipe->cancel != NULL && *pipe->cancel) ? "unlinked" : "timed out",
                                 pipe->setup.bmRequestType, pipe->setup.bRequest,
                                 pipe->setup.wValue, pipe->setup.wIndex);
                    }
                }
                if ((now - pipe->cancel_tick) < pdMS_TO_TICKS(USB_BACKEND_CANCEL_WAIT_MS)) {
                    continue;
                }
                ESP_LOGW(TAG, "%s: cancelled control transfer did not complete, holding its slot",
                         pipe->busid);
            }

            /* An aborted transfer may still be in flight after the halt/flush
               or cancel above.  Freeing it then lets the stack write into
               freed heap, so answer the caller now and hold the slot until
               the transfer really completes (the device answers, or is
               gone). */
            if (pipe->aborted && !pipe->completed) {
                orphan_transfer(pipe);
                continue;
            }

            /* A control transfer retired by an EP0 flush it did not ask for
               (another transfer's cancel, or an EP0 error on another
               transfer) never ran to completion: submit it again, as Linux
               keeps URBs queued behind an unlinked or failed one. */
            if (pipe->completed && !pipe->aborted && pipe->is_control
                && pipe->xfer->status == USB_TRANSFER_STATUS_CANCELED
                && pipe->resubmits < USB_BACKEND_MAX_CTRL_RESUBMITS) {
                pipe->resubmits++;
                ESP_LOGD(TAG, "%s: control transfer retired by an EP0 flush, resubmitting (%u)",
                         pipe->busid, pipe->resubmits);
                usb_host_transfer_free(pipe->xfer);
                pipe->xfer = NULL;
                pipe->completed = false;
                pipe->submitted = false;   /* Phase 1 submits it again */
                continue;
            }

            /* If the transfer has completed (callback fired) or we've
               aborted it, finalise and signal the waiting caller. */
            if (pipe->completed || pipe->aborted) {
                const usb_transfer_status_t usb_status =
                    pipe->completed ? pipe->xfer->status : USB_TRANSFER_STATUS_TIMED_OUT;
                int status = complete_transfer(pipe);

                /* If we forced the abort, override the hardware status
                   with the appropriate errno, unless the transfer finished
                   on its own before the abort took effect. */
                if (pipe->aborted && (!pipe->completed || usb_status == USB_TRANSFER_STATUS_CANCELED)) {
                    status = (pipe->cancel != NULL && *pipe->cancel)
                                 ? -ECONNRESET
                                 : -ETIMEDOUT;
                } else if (pipe->is_control) {
                    if (status == 0) {
                        apply_standard_request_to_host(pipe);
                    }
                } else if (status != 0 && pipe->dev_hdl != NULL) {
                    note_transfer_failure(pipe, usb_status);
                    /* URBs queued behind a STALL are flushed by the host
                       library; they failed because the endpoint halted. */
                    if (usb_status == USB_TRANSFER_STATUS_CANCELED && endpoint_is_halted(pipe)) {
                        status = -EPIPE;
                    }
                }

                pipe->active = false;
                pipe->submitted = false;
                if (pipe->status_out != NULL) {
                    *pipe->status_out = status;
                }
                xSemaphoreGive(pipe->done_sem);
            }
        }
    }
}

esp_err_t usb_backend_start(void)
{
    memset(&s_state, 0, sizeof(s_state));

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_USBIP_S3_USB_OTG_DEVKIT_POWER
    esp_err_t board_power_err = usb_backend_s3_usb_otg_devkit_power_init();
    if (board_power_err != ESP_OK) {
        return board_power_err;
    }
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
    esp_err_t p4hil_power_err = usb_backend_p4hil_usb_power_init();
    if (p4hil_power_err != ESP_OK) {
        return p4hil_power_err;
    }
#endif

    s_state.state_mutex = xSemaphoreCreateMutex();
    if (s_state.state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Counting semaphore to limit concurrent in-flight transfers to the
       number of hardware host channels. */
    s_state.pipe_avail_sem = xSemaphoreCreateCounting(USB_BACKEND_NUM_PIPES, 0);
    if (s_state.pipe_avail_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Release all slots into the pool. */
    for (int i = 0; i < USB_BACKEND_NUM_PIPES; i++) {
        s_state.pipes[i].done_sem = xSemaphoreCreateBinary();
        if (s_state.pipes[i].done_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_state.pipe_avail_sem);
    }

    s_state.event_queue = xQueueCreate(USB_BACKEND_EVENT_QUEUE_LEN, sizeof(usb_backend_event_t));
    if (s_state.event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = 0,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(usb_backend_daemon_task,
                    "usb_host_daemon",
                    USB_BACKEND_DAEMON_TASK_STACK,
                    NULL,
                    USB_BACKEND_DAEMON_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_FAIL;
    }

    if (xTaskCreate(usb_backend_task,
                    "usb_backend",
                    USB_BACKEND_TASK_STACK,
                    NULL,
                    USB_BACKEND_TASK_PRIORITY,
                    &s_state.task_hdl) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

size_t usb_backend_get_devices(usbip_backend_device_t *out_devices, size_t max_devices)
{
    if (out_devices == NULL || max_devices == 0) {
        return 0;
    }

    size_t copied = 0;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES && copied < max_devices; i++) {
        if (!s_state.devices[i].in_use) {
            continue;
        }

        out_devices[copied++] = s_state.devices[i].device;
    }
    xSemaphoreGive(s_state.state_mutex);

    /* Append virtual devices. */
    if (copied < max_devices) {
        copied += virtual_device_get_all(out_devices + copied, max_devices - copied);
    }

    return copied;
}

/* Hubs are owned by the USB Host Library and never reported to clients, so
   ask the library for them and rebuild each hub's port path from the parent
   links (a hub's parent is always another hub or the root port). */
size_t usb_backend_get_hubs(usb_backend_hub_t *out_hubs, size_t max_hubs)
{
    if (out_hubs == NULL || max_hubs == 0 || s_state.client_hdl == NULL) {
        return 0;
    }
    uint8_t addrs[USB_BACKEND_MAX_HUBS];
    size_t count = 0;
    if (usb_host_hub_list(addrs, USB_BACKEND_MAX_HUBS, &count) != ESP_OK) {
        return 0;
    }
    if (count > USB_BACKEND_MAX_HUBS) {
        count = USB_BACKEND_MAX_HUBS;
    }

    usb_host_hub_info_t infos[USB_BACKEND_MAX_HUBS];
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (usb_host_hub_get_info(addrs[i], &infos[n]) == ESP_OK) {
            n++;
        }
    }

    size_t copied = 0;
    for (size_t i = 0; i < n && copied < max_hubs; i++) {
        usb_backend_hub_t *hub = &out_hubs[copied++];
        memset(hub, 0, sizeof(*hub));
        hub->addr = infos[i].dev_addr;
        hub->parent_hub_addr = infos[i].parent_addr;
        hub->parent_port = infos[i].parent_port;
        hub->id_vendor = infos[i].vid;
        hub->id_product = infos[i].pid;
        strlcpy(hub->manufacturer, infos[i].manufacturer, sizeof(hub->manufacturer));
        strlcpy(hub->product, infos[i].product, sizeof(hub->product));

        /* Walk up the parent chain collecting port numbers */
        uint8_t ports[USB_BACKEND_MAX_HUB_DEPTH];
        size_t depth = 0;
        const usb_host_hub_info_t *cur = &infos[i];
        while (cur != NULL && cur->parent_addr != 0 && depth < USB_BACKEND_MAX_HUB_DEPTH) {
            ports[depth++] = cur->parent_port;
            const usb_host_hub_info_t *parent = NULL;
            for (size_t k = 0; k < n; k++) {
                if (infos[k].dev_addr == cur->parent_addr) {
                    parent = &infos[k];
                    break;
                }
            }
            cur = parent;
        }
        size_t off = strlcpy(hub->path, "1-1", sizeof(hub->path));
        while (depth > 0 && off < sizeof(hub->path)) {
            off += snprintf(hub->path + off, sizeof(hub->path) - off, ".%u", ports[--depth]);
        }
    }
    return copied;
}

bool usb_backend_get_device_by_busid(const char busid[32], usbip_backend_device_t *out_device)
{
    if (busid == NULL || out_device == NULL) {
        return false;
    }

    bool found = false;

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(busid);
    if (slot >= 0) {
        *out_device = s_state.devices[slot].device;
        found = true;
    }
    xSemaphoreGive(s_state.state_mutex);

    if (!found) {
        virtual_device_t *vdev = virtual_device_find_by_busid(busid);
        if (vdev != NULL) {
            *out_device = vdev->desc;
            found = true;
        }
    }

    return found;
}

/* Reserve a free pipe slot, fill it with the caller's parameters,
   wake the backend task, and wait for completion.  Each URB gets its
   own dynamically-allocated slot — concurrent URBs on the same
   endpoint no longer collide. */
static int submit_pipe_request(const char busid[32],
                               uint8_t endpoint_addr,
                               const usb_setup_packet_t *setup,
                               const uint8_t *out_data,
                               size_t out_len,
                               uint8_t *in_data,
                               size_t in_capacity,
                               size_t *in_len,
                               volatile bool *cancel)
{
    if (busid == NULL || in_len == NULL) {
        return -EINVAL;
    }

    *in_len = 0;

    /* Wait for a free host channel from the shared pool. */
    xSemaphoreTake(s_state.pipe_avail_sem, portMAX_DELAY);

    /* Allocate a specific slot under the mutex. */
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);

    const int dev_slot = find_slot_by_busid_locked(busid);
    if (dev_slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        xSemaphoreGive(s_state.pipe_avail_sem);
        return -ENODEV;
    }

    /* Verify the endpoint exists (non-EP0 only). */
    if (endpoint_addr != 0) {
        const usbip_backend_device_t *device = &s_state.devices[dev_slot].device;
        bool found = false;
        for (uint8_t i = 0; i < device->num_endpoints; i++) {
            if (device->endpoints[i].address == endpoint_addr) {
                found = true;
                break;
            }
        }
        if (!found) {
            xSemaphoreGive(s_state.state_mutex);
            xSemaphoreGive(s_state.pipe_avail_sem);
            ESP_LOGD(TAG, "Unknown endpoint 0x%02x on %s", endpoint_addr, busid);
            return -ENODEV;
        }
    }

    const int pipe_idx = alloc_pipe_locked();
    xSemaphoreGive(s_state.state_mutex);

    if (pipe_idx < 0) {
        /* Should never happen if the counting semaphore is correct. */
        xSemaphoreGive(s_state.pipe_avail_sem);
        ESP_LOGE(TAG, "No free pipe slot (semaphore accounting error)");
        return -EBUSY;
    }

    usb_backend_pipe_req_t *pipe = &s_state.pipes[pipe_idx];

    int status = -EIO;

    memcpy(pipe->busid, busid, sizeof(pipe->busid));
    pipe->endpoint_addr = endpoint_addr;
    if (setup != NULL) {
        pipe->setup = *setup;
    }
    pipe->out_data = out_data;
    pipe->out_len = out_len;
    pipe->in_data = in_data;
    pipe->in_capacity = in_capacity;
    pipe->in_len_out = in_len;
    pipe->status_out = &status;
    pipe->cancel = cancel;
    pipe->resubmits = 0;
    pipe->cancel_sent = false;
    pipe->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_BACKEND_XFER_TIMEOUT_MS);

    /* Mark active and wake the backend task. */
    pipe->active = true;
    xTaskNotifyGive(s_state.task_hdl);

    /* Wait for the backend task to process this slot. */
    xSemaphoreTake(pipe->done_sem, portMAX_DELAY);

    /* Return the pipe slot to the shared pool.  Only clear assigned;
       active/submitted are owned by the backend task.  An orphaned slot
       (aborted, transfer still in flight) is released by the backend task
       once the transfer completes. */
    bool give = true;
    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    if (pipe->orphaned) {
        pipe->caller_left = true;
        give = false;
    } else {
        pipe->assigned = false;
    }
    xSemaphoreGive(s_state.state_mutex);
    if (give) {
        xSemaphoreGive(s_state.pipe_avail_sem);
    }

    return status;
}

int usb_backend_control_transfer(const char busid[32],
                                 const usb_setup_packet_t *setup,
                                 const uint8_t *out_data,
                                 size_t out_len,
                                 uint8_t *in_data,
                                 size_t in_capacity,
                                 size_t *in_len,
                                 volatile bool *cancel)
{
    if (setup == NULL) {
        return -EINVAL;
    }
    return submit_pipe_request(busid, 0, setup,
                               out_data, out_len,
                               in_data, in_capacity, in_len,
                               cancel);
}

int usb_backend_bulk_transfer(const char busid[32],
                              uint8_t endpoint_addr,
                              const uint8_t *out_data,
                              size_t out_len,
                              uint8_t *in_data,
                              size_t in_capacity,
                              size_t *in_len,
                              volatile bool *cancel)
{
    return submit_pipe_request(busid, endpoint_addr, NULL,
                               out_data, out_len,
                               in_data, in_capacity, in_len,
                               cancel);
}

int usb_backend_interrupt_transfer(const char busid[32],
                                   uint8_t endpoint_addr,
                                   const uint8_t *out_data,
                                   size_t out_len,
                                   uint8_t *in_data,
                                   size_t in_capacity,
                                   size_t *in_len,
                                   volatile bool *cancel)
{
    return submit_pipe_request(busid, endpoint_addr, NULL,
                               out_data, out_len,
                               in_data, in_capacity, in_len,
                               cancel);
}

bool usb_backend_is_interrupt_endpoint(const char busid[32], uint8_t ep_num, uint8_t direction)
{
    if (busid == NULL) {
        return false;
    }

    const uint8_t ep_addr = ep_num | (direction ? 0x80 : 0x00);

    xSemaphoreTake(s_state.state_mutex, portMAX_DELAY);
    const int slot = find_slot_by_busid_locked(busid);
    if (slot < 0) {
        xSemaphoreGive(s_state.state_mutex);
        return false;
    }

    const usbip_backend_device_t *device = &s_state.devices[slot].device;
    for (uint8_t i = 0; i < device->num_endpoints; i++) {
        if (device->endpoints[i].address == ep_addr) {
            bool is_intr = (device->endpoints[i].attributes & 0x03) == 0x03;
            xSemaphoreGive(s_state.state_mutex);
            return is_intr;
        }
    }

    xSemaphoreGive(s_state.state_mutex);
    return false;
}
