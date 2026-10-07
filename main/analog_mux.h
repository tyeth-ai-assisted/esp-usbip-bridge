#ifndef ANALOG_MUX_H
#define ANALOG_MUX_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/*
 * Analog I2C strand mux: routes a shared I2C chain (SDA+SCL) to exactly one
 * DUT through I2C controlled analog switches.  A C port of
 * Gundry-Consultancy/sbc-dut-analog-mux-api-circuitpy (switches.py,
 * router.py, config.py) with the same topology format and semantics:
 *
 *   {"groups":[{"name":"muxA","type":"adg729","address":"0x44"},
 *              {"name":"muxB","type":"adg728_pair","sda_address":"0x4c","scl_address":"0x4d"}],
 *    "duts":[{"name":"dut-01","group":"muxA","channel":0}]}
 *
 * Errors follow the HTTP mapping of the original server:
 *   ESP_ERR_NOT_FOUND     unknown dut/group (404)
 *   ESP_ERR_INVALID_ARG   bad channel or topology (400)
 *   ESP_ERR_INVALID_STATE control bus unavailable or a write failed (503)
 *
 * All functions are thread safe.
 */

/* Load the saved topology, start the control bus and isolate every channel. */
esp_err_t analog_mux_init(void);

/* Whether the I2C control bus is available. */
bool analog_mux_bus_ok(void);

/* {active, manual, duts:[...], groups:[...]} - caller frees with cJSON_Delete. */
cJSON *analog_mux_describe(void);

/* Copy of the current topology - caller frees with cJSON_Delete. */
cJSON *analog_mux_get_topology(void);

/* {scan:["0x44",...], groups:[{name, addresses, present}]} - caller frees. */
cJSON *analog_mux_probe(void);

/* Connect `dut` (and only it) to the shared bus, break-before-make. */
esp_err_t analog_mux_select_dut(const char *dut, char *err, size_t err_len);

/* Exclusively connect group+channel, break-before-make.  `active` receives
   the DUT mapped to that channel, or "<group>:ch<n>". */
esp_err_t analog_mux_select_channel(const char *group, int channel,
                                    char *active, size_t active_len,
                                    char *err, size_t err_len);

/* Manually open/close one route (bring-up); marks the router manual. */
esp_err_t analog_mux_set_channel(const char *group, int channel, bool closed,
                                 char *err, size_t err_len);

/* Open every route on every group. */
esp_err_t analog_mux_isolate(char *err, size_t err_len);

/* Validate `topology` and hot-swap it in, isolating before and after.  The
   topology is persisted to NVS; `saved` reports whether that worked. */
esp_err_t analog_mux_apply_topology(const cJSON *topology, bool *saved, char *err, size_t err_len);

/* Name of the active DUT/channel ("" when isolated). */
void analog_mux_get_active(char *out, size_t out_len);

/* Names of the DUTs and groups, for error responses - caller frees. */
cJSON *analog_mux_dut_names(void);
cJSON *analog_mux_group_names(void);

#endif
