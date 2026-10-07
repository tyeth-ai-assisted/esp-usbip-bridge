#include "hub_control.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "hub_ctl";

#define HUB_CTL_CYCLE_QUEUE_LEN     8
#define HUB_CTL_CYCLE_TASK_STACK    4096
#define HUB_CTL_CYCLE_TASK_PRIO     4
#define HUB_CTL_MAX_OFF_MS          60000

#define HUB_CTL_NVS_NAMESPACE       "hubctl"
#define HUB_CTL_NVS_KEY_ENFORCE     "enforce"
#define HUB_CTL_NVS_KEY_RESTORE     "restore"
#define HUB_CTL_NVS_PORT_PREFIX     "p"      /* "p1-1.3" = port 1-1.3 kept off */
#define HUB_CTL_MAX_SAVED_PORTS     32
#define HUB_CTL_SAVED_PATH_LEN      15       /* NVS keys are at most 15 chars */

/* wPortStatus bits, USB 2.0 Table 11-21 */
#define PORT_STAT_CONNECTION    (1U << 0)
#define PORT_STAT_ENABLE        (1U << 1)
#define PORT_STAT_SUSPEND       (1U << 2)
#define PORT_STAT_OVER_CURRENT  (1U << 3)
#define PORT_STAT_RESET         (1U << 4)
#define PORT_STAT_POWER         (1U << 8)
#define PORT_STAT_LOW_SPEED     (1U << 9)
#define PORT_STAT_HIGH_SPEED    (1U << 10)

typedef struct {
    uint8_t hub_addr;
    uint8_t port;
    uint32_t off_ms;
    uint32_t flags;
    char path[32];
} hub_ctl_cycle_job_t;

static QueueHandle_t s_cycle_queue;
static SemaphoreHandle_t s_nvs_lock;

/* Settings and the ports to keep off.  Read by the port power policy on the
   USB Host Library task, so guarded by a spinlock rather than a mutex. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static hub_ctl_settings_t s_settings = { .enforce_per_port = true, .restore_on_reset = true };
static char s_off_ports[HUB_CTL_MAX_SAVED_PORTS][HUB_CTL_SAVED_PATH_LEN];
static size_t s_off_count;

const char *hub_ctl_power_switching_name(uint8_t power_switching)
{
    switch (power_switching) {
    case USB_HOST_HUB_POWER_SWITCHING_GANGED:
        return "ganged";
    case USB_HOST_HUB_POWER_SWITCHING_PER_PORT:
        return "per-port";
    default:
        return "none";
    }
}

const char *hub_ctl_over_current_name(uint8_t over_current_protection)
{
    switch (over_current_protection) {
    case 0:
        return "global";
    case 1:
        return "per-port";
    default:
        return "none";
    }
}

static void set_msg(char *msg, size_t msg_len, const char *fmt, ...)
{
    if (msg == NULL || msg_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, msg_len, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------------
 *  Saved port states and settings
 * ------------------------------------------------------------------------- */

static int off_index_locked(const char *path)
{
    for (size_t i = 0; i < s_off_count; i++) {
        if (strcmp(s_off_ports[i], path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool saved_on(const char *path)
{
    portENTER_CRITICAL(&s_lock);
    bool on = off_index_locked(path) < 0;
    portEXIT_CRITICAL(&s_lock);
    return on;
}

static void save_port_state(const char *path, bool on)
{
    if (strlen(path) >= HUB_CTL_SAVED_PATH_LEN) {
        ESP_LOGW(TAG, "Port path %s too long to save its state", path);
        return;
    }
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    int idx = off_index_locked(path);
    if (on && idx >= 0) {
        memmove(s_off_ports[idx], s_off_ports[idx + 1], (s_off_count - idx - 1) * HUB_CTL_SAVED_PATH_LEN);
        s_off_count--;
        changed = true;
    } else if (!on && idx < 0 && s_off_count < HUB_CTL_MAX_SAVED_PORTS) {
        strlcpy(s_off_ports[s_off_count++], path, HUB_CTL_SAVED_PATH_LEN);
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!changed) {
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), HUB_CTL_NVS_PORT_PREFIX "%.14s", path);
    xSemaphoreTake(s_nvs_lock, portMAX_DELAY);
    nvs_handle_t h;
    if (nvs_open(HUB_CTL_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (on) {
            nvs_erase_key(h, key);
        } else {
            nvs_set_u8(h, key, 0);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_lock);
}

void hub_ctl_get_settings(hub_ctl_settings_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_settings;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t hub_ctl_set_settings(const hub_ctl_settings_t *settings)
{
    portENTER_CRITICAL(&s_lock);
    s_settings = *settings;
    portEXIT_CRITICAL(&s_lock);

    xSemaphoreTake(s_nvs_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(HUB_CTL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_u8(h, HUB_CTL_NVS_KEY_ENFORCE, settings->enforce_per_port);
        nvs_set_u8(h, HUB_CTL_NVS_KEY_RESTORE, settings->restore_on_reset);
        err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_lock);
    ESP_LOGI(TAG, "Settings: enforce per-port switching %s, restore port power on hub reset %s",
             settings->enforce_per_port ? "on" : "off", settings->restore_on_reset ? "on" : "off");
    return err;
}

/* Port power policy: keep saved-off ports off when their hub enumerates.
   Runs on the USB Host Library task. */
static bool port_policy_cb(const usb_host_hub_port_path_t *path, void *arg)
{
    (void)arg;
    char key[HUB_CTL_SAVED_PATH_LEN] = "1-1";
    size_t off = 3;
    for (uint8_t i = 0; i < path->depth && off < sizeof(key); i++) {
        off += snprintf(key + off, sizeof(key) - off, ".%u", path->ports[i]);
    }
    portENTER_CRITICAL(&s_lock);
    bool keep_off = s_settings.restore_on_reset && off_index_locked(key) >= 0;
    portEXIT_CRITICAL(&s_lock);
    return !keep_off;
}

esp_err_t hub_ctl_init(void)
{
    s_nvs_lock = xSemaphoreCreateMutex();
    if (s_nvs_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t h;
    if (nvs_open(HUB_CTL_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, HUB_CTL_NVS_KEY_ENFORCE, &v) == ESP_OK) {
            s_settings.enforce_per_port = v;
        }
        if (nvs_get_u8(h, HUB_CTL_NVS_KEY_RESTORE, &v) == ESP_OK) {
            s_settings.restore_on_reset = v;
        }
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find_in_handle(h, NVS_TYPE_U8, &it);
        while (err == ESP_OK && s_off_count < HUB_CTL_MAX_SAVED_PORTS) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.key[0] == HUB_CTL_NVS_PORT_PREFIX[0] && info.key[1] >= '0' && info.key[1] <= '9') {
                strlcpy(s_off_ports[s_off_count++], info.key + 1, HUB_CTL_SAVED_PATH_LEN);
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(h);
    }

    usb_host_hub_set_port_policy(port_policy_cb, NULL);
    ESP_LOGI(TAG, "Enforce per-port switching %s, restore on hub reset %s, %u port(s) saved off",
             s_settings.enforce_per_port ? "on" : "off", s_settings.restore_on_reset ? "on" : "off",
             (unsigned)s_off_count);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Topology helpers
 * ------------------------------------------------------------------------- */

/* Split "1-1.3" into hub path "1-1" and port 3. */
static bool split_port_path(const char *port_path, char *hub_path, size_t hub_path_size, uint8_t *port)
{
    if (port_path == NULL) {
        return false;
    }
    const char *dot = strrchr(port_path, '.');
    if (dot == NULL || dot == port_path || (size_t)(dot - port_path) >= hub_path_size) {
        return false;
    }
    char *end = NULL;
    long p = strtol(dot + 1, &end, 10);
    if (end == dot + 1 || *end != '\0' || p < 1 || p > 255) {
        return false;
    }
    memcpy(hub_path, port_path, dot - port_path);
    hub_path[dot - port_path] = '\0';
    *port = (uint8_t)p;
    return true;
}

static bool find_hub_by_path(const char *hub_path, usb_backend_hub_t *out)
{
    usb_backend_hub_t hubs[USB_BACKEND_MAX_HUBS];
    size_t n = usb_backend_get_hubs(hubs, USB_BACKEND_MAX_HUBS);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(hubs[i].path, hub_path) == 0) {
            *out = hubs[i];
            return true;
        }
    }
    return false;
}

static bool port_has_hub(uint8_t hub_addr, uint8_t port)
{
    usb_backend_hub_t hubs[USB_BACKEND_MAX_HUBS];
    size_t n = usb_backend_get_hubs(hubs, USB_BACKEND_MAX_HUBS);
    for (size_t i = 0; i < n; i++) {
        if (hubs[i].parent_hub_addr == hub_addr && hubs[i].parent_port == port) {
            return true;
        }
    }
    return false;
}

size_t hub_ctl_snapshot(hub_ctl_hub_status_t *out, size_t max_hubs)
{
    usb_backend_hub_t hubs[USB_BACKEND_MAX_HUBS];
    size_t n_hubs = usb_backend_get_hubs(hubs, USB_BACKEND_MAX_HUBS);
    if (n_hubs > max_hubs) {
        n_hubs = max_hubs;
    }

    size_t devs_max = CONFIG_USBIP_MAX_DEVICES;
    usbip_backend_device_t *devs = malloc(devs_max * sizeof(*devs));
    size_t n_devs = devs ? usb_backend_get_devices(devs, devs_max) : 0;

    for (size_t h = 0; h < n_hubs; h++) {
        hub_ctl_hub_status_t *st = &out[h];
        memset(st, 0, sizeof(*st));
        st->hub = hubs[h];

        /* One request for the hub and all of its ports */
        usb_host_hub_port_info_t pinfo[HUB_CTL_MAX_PORTS];
        size_t n_ports = 0;
        st->info_ok = (usb_host_hub_get_snapshot(hubs[h].addr, &st->info, pinfo, HUB_CTL_MAX_PORTS,
                                                 &n_ports) == ESP_OK);
        if (!st->info_ok) {
            continue;
        }
        st->num_ports = (uint8_t)n_ports;
        for (uint8_t p = 1; p <= st->num_ports; p++) {
            hub_ctl_port_t *port = &st->ports[p - 1];
            const usb_host_hub_port_info_t *pi = &pinfo[p - 1];
            port->port = p;
            snprintf(port->path, sizeof(port->path), "%.27s.%u", hubs[h].path, p);
            port->powered = pi->port_status & PORT_STAT_POWER;
            port->connected = pi->port_status & PORT_STAT_CONNECTION;
            port->enabled = pi->port_status & PORT_STAT_ENABLE;
            port->suspended = pi->port_status & PORT_STAT_SUSPEND;
            port->over_current = pi->port_status & PORT_STAT_OVER_CURRENT;
            port->resetting = pi->port_status & PORT_STAT_RESET;
            port->user_off = pi->user_power_off;
            port->desired_on = saved_on(port->path);
            port->mismatch = port->powered != port->desired_on;
            if (port->connected) {
                port->speed = (pi->port_status & PORT_STAT_LOW_SPEED) ? "low"
                            : (pi->port_status & PORT_STAT_HIGH_SPEED) ? "high" : "full";
            }
            for (size_t d = 0; d < n_devs; d++) {
                if (devs[d].parent_hub_addr == hubs[h].addr && devs[d].parent_port == p) {
                    port->has_device = true;
                    port->id_vendor = devs[d].id_vendor;
                    port->id_product = devs[d].id_product;
                    break;
                }
            }
            for (size_t k = 0; k < n_hubs; k++) {
                if (hubs[k].parent_hub_addr == hubs[h].addr && hubs[k].parent_port == p) {
                    port->has_hub = true;
                    port->id_vendor = hubs[k].id_vendor;
                    port->id_product = hubs[k].id_product;
                    break;
                }
            }
        }
    }
    free(devs);
    return n_hubs;
}

/* ---------------------------------------------------------------------------
 *  Switching
 * ------------------------------------------------------------------------- */

/* Validate a request and resolve the hub address, port and library flags. */
static esp_err_t check_port_request(const char *port_path, bool on, bool force,
                                    uint8_t *hub_addr, uint8_t *port, uint32_t *flags,
                                    char *msg, size_t msg_len)
{
    char hub_path[32];
    if (!split_port_path(port_path, hub_path, sizeof(hub_path), port)) {
        if (port_path != NULL && strchr(port_path, '.') == NULL) {
            set_msg(msg, msg_len, "%s is the root port; its power is not switchable", port_path);
            return ESP_ERR_NOT_SUPPORTED;
        }
        set_msg(msg, msg_len, "invalid port path, expected e.g. 1-1.3");
        return ESP_ERR_INVALID_ARG;
    }
    usb_backend_hub_t hub;
    if (!find_hub_by_path(hub_path, &hub)) {
        set_msg(msg, msg_len, "no hub at %s", hub_path);
        return ESP_ERR_NOT_FOUND;
    }
    usb_host_hub_info_t info;
    esp_err_t err = usb_host_hub_get_info(hub.addr, &info);
    if (err != ESP_OK) {
        set_msg(msg, msg_len, "hub %s not ready: %s", hub_path, esp_err_to_name(err));
        return err;
    }
    if (*port > info.num_ports) {
        set_msg(msg, msg_len, "hub %s has %u ports", hub_path, info.num_ports);
        return ESP_ERR_INVALID_SIZE;
    }
    hub_ctl_settings_t settings;
    hub_ctl_get_settings(&settings);
    const bool per_port = (info.power_switching == USB_HOST_HUB_POWER_SWITCHING_PER_PORT);
    if (!per_port && settings.enforce_per_port && !force) {
        set_msg(msg, msg_len, "hub %s reports %s power switching; this may switch all ports or nothing "
                "(pass force, or turn off per-port switching enforcement)", hub_path,
                hub_ctl_power_switching_name(info.power_switching));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!force && !on && port_has_hub(hub.addr, *port)) {
        set_msg(msg, msg_len, "port %s leads to another hub; powering it off removes every device "
                "behind it (pass force to do it anyway)", port_path);
        return ESP_ERR_INVALID_STATE;
    }
    *hub_addr = hub.addr;
    *flags = per_port ? 0 : USB_HOST_HUB_PORT_POWER_FLAG_FORCE;
    return ESP_OK;
}

esp_err_t hub_ctl_port_power(const char *port_path, bool on, bool force, char *msg, size_t msg_len)
{
    uint8_t hub_addr = 0;
    uint8_t port = 0;
    uint32_t flags = 0;
    esp_err_t err = check_port_request(port_path, on, force, &hub_addr, &port, &flags, msg, msg_len);
    if (err != ESP_OK) {
        return err;
    }
    err = usb_host_hub_port_power(hub_addr, port, on, flags);
    if (err != ESP_OK) {
        set_msg(msg, msg_len, "power %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
        return err;
    }
    save_port_state(port_path, on);
    ESP_LOGI(TAG, "Port %s powered %s", port_path, on ? "on" : "off");
    return ESP_OK;
}

static void cycle_task(void *arg)
{
    (void)arg;
    hub_ctl_cycle_job_t job;
    while (true) {
        if (xQueueReceive(s_cycle_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = usb_host_hub_port_power(job.hub_addr, job.port, false, job.flags);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cycle %s: power off failed: %s", job.path, esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(job.off_ms));
        err = usb_host_hub_port_power(job.hub_addr, job.port, true, job.flags);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cycle %s: power on failed: %s", job.path, esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "Port %s power cycled (%" PRIu32 " ms off)", job.path, job.off_ms);
    }
}

esp_err_t hub_ctl_port_cycle(const char *port_path, uint32_t off_ms, bool force, char *msg, size_t msg_len)
{
    hub_ctl_cycle_job_t job = {0};
    esp_err_t err = check_port_request(port_path, false, force, &job.hub_addr, &job.port, &job.flags,
                                       msg, msg_len);
    if (err != ESP_OK) {
        return err;
    }
    if (off_ms > HUB_CTL_MAX_OFF_MS) {
        set_msg(msg, msg_len, "off_ms is limited to %d", HUB_CTL_MAX_OFF_MS);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cycle_queue == NULL) {
        s_cycle_queue = xQueueCreate(HUB_CTL_CYCLE_QUEUE_LEN, sizeof(hub_ctl_cycle_job_t));
        if (s_cycle_queue == NULL ||
                xTaskCreate(cycle_task, "hub_cycle", HUB_CTL_CYCLE_TASK_STACK, NULL,
                            HUB_CTL_CYCLE_TASK_PRIO, NULL) != pdPASS) {
            set_msg(msg, msg_len, "out of memory");
            return ESP_ERR_NO_MEM;
        }
    }
    job.off_ms = off_ms;
    strlcpy(job.path, port_path, sizeof(job.path));
    if (xQueueSend(s_cycle_queue, &job, 0) != pdTRUE) {
        set_msg(msg, msg_len, "too many power cycles queued");
        return ESP_ERR_NO_MEM;
    }
    /* A cycle ends powered on */
    save_port_state(port_path, true);
    return ESP_OK;
}

int hub_ctl_all_ports(bool on)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        return -1;
    }
    int switched = 0;
    size_t n = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
    for (size_t h = 0; h < n; h++) {
        if (!snap[h].info_ok || snap[h].info.power_switching != USB_HOST_HUB_POWER_SWITCHING_PER_PORT) {
            continue;
        }
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            if (snap[h].ports[p].has_hub) {
                continue;
            }
            if (usb_host_hub_port_power(snap[h].hub.addr, p + 1, on, 0) == ESP_OK) {
                save_port_state(snap[h].ports[p].path, on);
                switched++;
            }
        }
    }
    free(snap);
    return switched;
}

int hub_ctl_restore_now(void)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        return -1;
    }
    int switched = 0;
    size_t n = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
    for (size_t h = 0; h < n; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            const hub_ctl_port_t *port = &snap[h].ports[p];
            if (!port->mismatch) {
                continue;
            }
            /* The saved state was accepted when it was set, so apply it as is */
            if (usb_host_hub_port_power(snap[h].hub.addr, port->port, port->desired_on,
                                        USB_HOST_HUB_PORT_POWER_FLAG_FORCE) == ESP_OK) {
                ESP_LOGI(TAG, "Port %s restored %s", port->path, port->desired_on ? "on" : "off");
                switched++;
            }
        }
    }
    free(snap);
    return switched;
}
