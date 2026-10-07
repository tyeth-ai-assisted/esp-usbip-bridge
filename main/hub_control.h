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
    bool user_off;              /* switched off through hub_ctl_port_power() or the restore policy */
    bool desired_on;            /* saved power state for this port path (default on) */
    bool mismatch;              /* powered != desired_on */
    uint32_t enum_timeout_ms;   /* effective enumeration timeout for a device on this port (0 = none) */
    bool enum_timeout_override; /* set for this exact port path */
    const char *pending;        /* "on", "off", "cycle" while a request is queued, else NULL */
    const char *speed;          /* "low", "full", "high" or NULL when nothing is connected */
    bool has_device;            /* an exported device is on this port */
    bool has_hub;               /* a hub is on this port */
    uint16_t id_vendor;         /* of the device or hub on the port */
    uint16_t id_product;
} hub_ctl_port_t;

typedef struct {
    usb_backend_hub_t hub;
    uint32_t enum_timeout_ms;   /* effective timeout inherited by its ports */
    bool enum_timeout_override; /* set for this hub's path */
    bool info_ok;               /* false when the hub is still being configured */
    usb_host_hub_info_t info;
    uint8_t num_ports;
    hub_ctl_port_t ports[HUB_CTL_MAX_PORTS];
} hub_ctl_hub_status_t;

typedef struct {
    /* Refuse to switch ports of hubs that do not report per-port power
       switching (ganged / none) unless the request is forced. */
    bool enforce_per_port;
    /* Keep ports that were switched off powered off when their hub
       (re-)enumerates: hub reset, upstream power loss, bridge reboot. */
    bool restore_on_reset;
    /* Default enumeration control transfer timeout, ms (0 = none).  A device
       that does not answer in time has its port disabled so it does not stop
       other devices from enumerating. */
    uint32_t enum_timeout_ms;
} hub_ctl_settings_t;

#define HUB_CTL_MAX_TIMEOUT_OVERRIDES 32

typedef struct {
    char path[16];
    uint32_t timeout_ms;
} hub_ctl_timeout_override_t;

/* Load the settings and saved port states and install the port power policy.
   Call before usb_backend_start() so the first enumeration is covered. */
esp_err_t hub_ctl_init(void);

void hub_ctl_get_settings(hub_ctl_settings_t *out);
esp_err_t hub_ctl_set_settings(const hub_ctl_settings_t *settings);

/* Per-path enumeration timeout overrides.  An override on a path applies to
   the device at that path and to everything behind it (a hub path covers the
   hub's ports); the most specific path wins over the global setting.
   `timeout_ms` < 0 removes the override. */
esp_err_t hub_ctl_set_enum_timeout(const char *path, int32_t timeout_ms);
size_t hub_ctl_get_enum_timeouts(hub_ctl_timeout_override_t *out, size_t max);
/* Effective timeout for a device at `path` (`is_override` may be NULL). */
uint32_t hub_ctl_enum_timeout_for(const char *path, bool *is_override);

/* Names of wHubCharacteristics fields. */
const char *hub_ctl_power_switching_name(uint8_t power_switching);
const char *hub_ctl_over_current_name(uint8_t over_current_protection);

/* Snapshot every attached hub with its port states.  Must not be called from
   the USB Host Library task. */
size_t hub_ctl_snapshot(hub_ctl_hub_status_t *out, size_t max_hubs);

/* Switch a downstream hub port on or off.  `port_path` is the port's path,
   e.g. "1-1.3" (port 3 of the hub at "1-1").  Unless `force` is set the
   request is refused for hubs without per-port power switching (when that is
   enforced) and, when powering off, for ports that lead to another hub.  The
   new state is saved for the port path.  If the port is busy (e.g. its device
   is enumerating) the request is queued and applied in the background without
   blocking: `pending` (may be NULL) reports that.  On failure `msg` describes
   why. */
esp_err_t hub_ctl_port_power(const char *port_path, bool on, bool force, bool *pending,
                             char *msg, size_t msg_len);

/* Power a port off, wait `off_ms`, and power it back on, in the background
   (each port independently).  Validation happens before returning. */
esp_err_t hub_ctl_port_cycle(const char *port_path, uint32_t off_ms, bool force, char *msg, size_t msg_len);

/* Power off (or on) every port of every per-port switched hub that does not
   lead to another hub.  Returns the number of ports switched. */
int hub_ctl_all_ports(bool on);

/* Apply the saved state to every port whose power does not match it.
   Returns the number of ports switched, or -1 on error. */
int hub_ctl_restore_now(void);

#endif
