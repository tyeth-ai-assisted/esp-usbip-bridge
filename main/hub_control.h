#ifndef HUB_CONTROL_H
#define HUB_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/usb_host_hub.h"

#include "usb_backend.h"

#define HUB_CTL_MAX_PORTS 15

typedef struct {
    uint8_t port;
    char path[32];              /* port path, e.g. "1-1.3" (the busid of a device on it) */
    bool powered;
    bool connected;
    bool enabled;
    bool suspended;
    bool over_current;
    bool resetting;
    bool user_off;              /* switched off through hub_ctl_port_power() */
    const char *speed;          /* "low", "full", "high" or NULL when nothing is connected */
    bool has_device;            /* an exported device is on this port */
    bool has_hub;               /* a hub is on this port */
    uint16_t id_vendor;         /* of the device or hub on the port */
    uint16_t id_product;
} hub_ctl_port_t;

typedef struct {
    usb_backend_hub_t hub;
    bool info_ok;               /* false when the hub is still being configured */
    usb_host_hub_info_t info;
    uint8_t num_ports;
    hub_ctl_port_t ports[HUB_CTL_MAX_PORTS];
} hub_ctl_hub_status_t;

/* Name of a usb_host_hub_info_t.power_switching value. */
const char *hub_ctl_power_switching_name(uint8_t power_switching);

/* Snapshot every attached hub with its port states.  Must not be called from
   the USB Host Library task. */
size_t hub_ctl_snapshot(hub_ctl_hub_status_t *out, size_t max_hubs);

/* Switch a downstream hub port on or off.  `port_path` is the port's path,
   e.g. "1-1.3" (port 3 of the hub at "1-1").  Unless `force` is set the
   request is refused for hubs without per-port power switching and, when
   powering off, for ports that lead to another hub.  On failure `msg`
   describes why. */
esp_err_t hub_ctl_port_power(const char *port_path, bool on, bool force, char *msg, size_t msg_len);

/* Power a port off, wait `off_ms`, and power it back on, in the background.
   Validation happens before returning. */
esp_err_t hub_ctl_port_cycle(const char *port_path, uint32_t off_ms, bool force, char *msg, size_t msg_len);

/* Power off (or on) every port of every per-port switched hub that does not
   lead to another hub.  Returns the number of ports switched. */
int hub_ctl_all_ports(bool on);

#endif
