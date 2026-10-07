#include "hub_control.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

static const char *TAG = "hub_ctl";

#define HUB_CTL_CYCLE_QUEUE_LEN     8
#define HUB_CTL_CYCLE_TASK_STACK    4096
#define HUB_CTL_CYCLE_TASK_PRIO     4
#define HUB_CTL_MAX_OFF_MS          60000

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
    char path[32];
} hub_ctl_cycle_job_t;

static QueueHandle_t s_cycle_queue;

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
        st->info_ok = (usb_host_hub_get_info(hubs[h].addr, &st->info) == ESP_OK);
        if (!st->info_ok) {
            continue;
        }
        st->num_ports = st->info.num_ports > HUB_CTL_MAX_PORTS ? HUB_CTL_MAX_PORTS : st->info.num_ports;
        for (uint8_t p = 1; p <= st->num_ports; p++) {
            hub_ctl_port_t *port = &st->ports[p - 1];
            port->port = p;
            snprintf(port->path, sizeof(port->path), "%.27s.%u", hubs[h].path, p);

            usb_host_hub_port_info_t pi;
            if (usb_host_hub_get_port_info(hubs[h].addr, p, &pi) == ESP_OK) {
                port->powered = pi.port_status & PORT_STAT_POWER;
                port->connected = pi.port_status & PORT_STAT_CONNECTION;
                port->enabled = pi.port_status & PORT_STAT_ENABLE;
                port->suspended = pi.port_status & PORT_STAT_SUSPEND;
                port->over_current = pi.port_status & PORT_STAT_OVER_CURRENT;
                port->resetting = pi.port_status & PORT_STAT_RESET;
                port->user_off = pi.user_power_off;
                if (port->connected) {
                    port->speed = (pi.port_status & PORT_STAT_LOW_SPEED) ? "low"
                                : (pi.port_status & PORT_STAT_HIGH_SPEED) ? "high" : "full";
                }
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

/* Validate a request and resolve the hub address and port. */
static esp_err_t check_port_request(const char *port_path, bool on, bool force,
                                    uint8_t *hub_addr, uint8_t *port,
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
    if (!force && info.power_switching != USB_HOST_HUB_POWER_SWITCHING_PER_PORT) {
        set_msg(msg, msg_len, "hub %s reports %s power switching; this may switch all ports or nothing "
                "(pass force to send anyway)", hub_path, hub_ctl_power_switching_name(info.power_switching));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!force && !on && port_has_hub(hub.addr, *port)) {
        set_msg(msg, msg_len, "port %s leads to another hub; powering it off removes every device "
                "behind it (pass force to do it anyway)", port_path);
        return ESP_ERR_INVALID_STATE;
    }
    *hub_addr = hub.addr;
    return ESP_OK;
}

esp_err_t hub_ctl_port_power(const char *port_path, bool on, bool force, char *msg, size_t msg_len)
{
    uint8_t hub_addr = 0;
    uint8_t port = 0;
    esp_err_t err = check_port_request(port_path, on, force, &hub_addr, &port, msg, msg_len);
    if (err != ESP_OK) {
        return err;
    }
    err = usb_host_hub_port_power(hub_addr, port, on);
    if (err != ESP_OK) {
        set_msg(msg, msg_len, "power %s failed: %s", on ? "on" : "off", esp_err_to_name(err));
        return err;
    }
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
        esp_err_t err = usb_host_hub_port_power(job.hub_addr, job.port, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Cycle %s: power off failed: %s", job.path, esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(job.off_ms));
        err = usb_host_hub_port_power(job.hub_addr, job.port, true);
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
    esp_err_t err = check_port_request(port_path, false, force, &job.hub_addr, &job.port, msg, msg_len);
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
            if (usb_host_hub_port_power(snap[h].hub.addr, p + 1, on) == ESP_OK) {
                switched++;
            }
        }
    }
    free(snap);
    return switched;
}
