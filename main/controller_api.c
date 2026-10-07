#include "controller_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "analog_mux.h"
#include "device_naming.h"
#include "discovery_service.h"
#include "hub_control.h"
#include "usb_backend.h"
#include "virtual_device.h"
#include "version_autogen.h"

static const char *TAG = "controller_api";

#define API_NVS_NAMESPACE   "api"
#define API_NVS_KEY_TOKEN   "token"
#define API_TOKEN_MAX_LEN   64
#define API_DEFAULT_OFF_MS  1000
#define API_REBOOT_DELAY_US (800 * 1000)

#define MCP_PROTOCOL_VERSION "2025-06-18"

static char s_token[API_TOKEN_MAX_LEN + 1];

/* ===========================================================================
 *  Results
 * ======================================================================== */

typedef struct {
    int status;
    cJSON *json;
} api_result_t;

static api_result_t result(int status, cJSON *json)
{
    return (api_result_t){ .status = status, .json = json };
}

/* {"ok":false,"error":msg} */
static api_result_t fail(int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    return result(status, j);
}

/* {"error":msg}, the analog mux API's error shape */
static api_result_t mux_fail(int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return result(status, j);
}

static int status_for_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 200;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return 400;
    case ESP_ERR_NOT_FOUND:
        return 404;
    case ESP_ERR_NOT_SUPPORTED:
        return 409;
    case ESP_ERR_INVALID_STATE:
        return 409;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NO_MEM:
        return 503;
    default:
        return 500;
    }
}

const char *controller_api_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

/* ===========================================================================
 *  Auth
 * ======================================================================== */

esp_err_t controller_api_init(void)
{
    nvs_handle_t h;
    s_token[0] = '\0';
    if (nvs_open(API_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_token);
        if (nvs_get_str(h, API_NVS_KEY_TOKEN, s_token, &len) != ESP_OK) {
            s_token[0] = '\0';
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "API token %s", s_token[0] ? "set: mutating requests need Authorization: Bearer" : "not set");
    return ESP_OK;
}

bool controller_api_authorized(const char *auth)
{
    if (s_token[0] == '\0') {
        return true;
    }
    if (auth == NULL || strncasecmp(auth, "Bearer ", 7) != 0) {
        return false;
    }
    const char *given = auth + 7;
    while (*given == ' ') {
        given++;
    }
    size_t n = strlen(s_token);
    if (strlen(given) != n) {
        return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)(given[i] ^ s_token[i]);
    }
    return diff == 0;
}

static esp_err_t set_token(const char *token)
{
    if (strlen(token) > API_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(API_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = token[0] ? nvs_set_str(h, API_NVS_KEY_TOKEN, token) : nvs_erase_key(h, API_NVS_KEY_TOKEN);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s_token, token, sizeof(s_token));
    }
    return err;
}

/* ===========================================================================
 *  Helpers
 * ======================================================================== */

static const char *speed_name(uint32_t usbip_speed)
{
    switch (usbip_speed) {
    case 1: return "low";
    case 2: return "full";
    case 3: return "high";
    default: return "unknown";
    }
}

static int speed_mbps(uint32_t usbip_speed)
{
    switch (usbip_speed) {
    case 1: return 1;      /* 1.5 */
    case 2: return 12;
    case 3: return 480;
    default: return 0;
    }
}

static void add_hex(cJSON *obj, const char *key, unsigned value, int digits)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%0*x", digits, value);
    cJSON_AddStringToObject(obj, key, buf);
}

static void add_str_or_null(cJSON *obj, const char *key, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        cJSON_AddStringToObject(obj, key, value);
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

/* Percent-decode `in` into `out`. */
static void url_decode(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        if (in[i] == '%' && i + 2 < in_len && isxdigit((unsigned char)in[i + 1]) &&
                isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* Copy the path segment starting at `p` (up to '/' or end) decoded into out.
   Returns a pointer to the rest of the path. */
static const char *take_segment(const char *p, char *out, size_t out_size)
{
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    url_decode(p, len, out, out_size);
    return slash ? slash : p + len;
}

static bool query_flag(const char *query, const char *name)
{
    if (query == NULL) {
        return false;
    }
    size_t n = strlen(name);
    for (const char *q = query; q && *q; ) {
        if (strncmp(q, name, n) == 0 && (q[n] == '\0' || q[n] == '&' || q[n] == '=')) {
            if (q[n] != '=') {
                return true;
            }
            const char *v = q + n + 1;
            return !(v[0] == '0' || strncasecmp(v, "false", 5) == 0 || strncasecmp(v, "no", 2) == 0);
        }
        q = strchr(q, '&');
        if (q) {
            q++;
        }
    }
    return false;
}

static int query_int(const char *query, const char *name, int def)
{
    if (query == NULL) {
        return def;
    }
    size_t n = strlen(name);
    for (const char *q = query; q && *q; ) {
        if (strncmp(q, name, n) == 0 && q[n] == '=') {
            return atoi(q + n + 1);
        }
        q = strchr(q, '&');
        if (q) {
            q++;
        }
    }
    return def;
}

static bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0;
    }
    if (cJSON_IsString(v)) {
        return strcasecmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0 ||
               strcasecmp(v->valuestring, "on") == 0;
    }
    return def;
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool json_int(const cJSON *obj, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        *out = (int)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring[0] != '\0') {
        char *end = NULL;
        long n = strtol(v->valuestring, &end, 10);
        if (*end == '\0') {
            *out = (int)n;
            return true;
        }
    }
    return false;
}

/* ===========================================================================
 *  Core operations (shared by REST and MCP)
 * ======================================================================== */

static api_result_t core_info(void)
{
    cJSON *j = cJSON_CreateObject();
    char buf[DEVICE_NAME_MAX_LEN];
    device_naming_get_hostname(buf, sizeof(buf));
    cJSON_AddStringToObject(j, "hostname", buf);
    if (device_naming_get_board_id(buf, sizeof(buf)) == ESP_OK) {
        cJSON_AddStringToObject(j, "board_id", buf);
    } else {
        cJSON_AddNullToObject(j, "board_id");
    }
    cJSON_AddStringToObject(j, "target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(j, "version", HARNESS_FW_VERSION);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
    cJSON_AddBoolToObject(j, "mux_bus_ok", analog_mux_bus_ok());
    cJSON_AddNumberToObject(j, "usbip_port", 3240);
    return result(200, j);
}

static hub_ctl_hub_status_t *snapshot_hubs(size_t *n)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    *n = snap ? hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS) : 0;
    return snap;
}

static const hub_ctl_port_t *find_port(const hub_ctl_hub_status_t *snap, size_t n, const char *path)
{
    for (size_t h = 0; h < n; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            if (strcmp(snap[h].ports[p].path, path) == 0) {
                return &snap[h].ports[p];
            }
        }
    }
    return NULL;
}

static cJSON *device_json(const usbip_backend_device_t *d, const hub_ctl_hub_status_t *snap, size_t n_hubs)
{
    cJSON *o = cJSON_CreateObject();
    const bool is_virtual = (d->busnum == VIRTUAL_DEVICE_BUSNUM);
    cJSON_AddStringToObject(o, "busid", d->busid);
    add_hex(o, "vid", d->id_vendor, 4);
    add_hex(o, "pid", d->id_product, 4);

    char desc[160];
    snprintf(desc, sizeof(desc), "%04x:%04x%s%s%s%s", d->id_vendor, d->id_product,
             d->manufacturer[0] ? " " : "", d->manufacturer,
             d->product[0] ? " " : "", d->product);
    cJSON_AddStringToObject(o, "description", desc);
    add_str_or_null(o, "manufacturer", d->manufacturer);
    add_str_or_null(o, "product", d->product);
    add_str_or_null(o, "serial", d->serial);

    char name[DEVICE_NAME_MAX_LEN] = "";
    device_naming_get_device_name(d->busid, name, sizeof(name));
    add_str_or_null(o, "name", name);

    cJSON_AddStringToObject(o, "speed", speed_name(d->speed));
    cJSON_AddNumberToObject(o, "speed_mbps", speed_mbps(d->speed));
    if (d->max_power_ma) {
        cJSON_AddNumberToObject(o, "max_power_ma", d->max_power_ma);
    } else {
        cJSON_AddNullToObject(o, "max_power_ma");
    }
    char cls[8];
    snprintf(cls, sizeof(cls), "0x%02x", d->device_class);
    cJSON_AddStringToObject(o, "device_class", cls);
    snprintf(cls, sizeof(cls), "0x%04x", d->bcd_device);
    cJSON_AddStringToObject(o, "bcd_device", cls);
    cJSON_AddNumberToObject(o, "num_interfaces", d->num_interfaces);
    cJSON *ifs = cJSON_AddArrayToObject(o, "interfaces");
    for (uint8_t i = 0; i < d->num_interfaces; i++) {
        cJSON *it = cJSON_CreateObject();
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_class);
        cJSON_AddStringToObject(it, "class", cls);
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_subclass);
        cJSON_AddStringToObject(it, "subclass", cls);
        snprintf(cls, sizeof(cls), "0x%02x", d->interfaces[i].interface_protocol);
        cJSON_AddStringToObject(it, "protocol", cls);
        cJSON_AddItemToArray(ifs, it);
    }
    cJSON_AddBoolToObject(o, "virtual", is_virtual);
    if (is_virtual) {
        cJSON_AddNullToObject(o, "address");
        cJSON_AddNullToObject(o, "hub");
        cJSON_AddNullToObject(o, "port");
    } else {
        cJSON_AddNumberToObject(o, "address", d->dev_addr);
        if (d->parent_hub_addr) {
            /* The hub path is the busid without its last ".N" */
            char hub_path[32];
            strlcpy(hub_path, d->busid, sizeof(hub_path));
            char *dot = strrchr(hub_path, '.');
            if (dot) {
                *dot = '\0';
            }
            cJSON_AddStringToObject(o, "hub", hub_path);
            cJSON_AddNumberToObject(o, "port", d->parent_port);
        } else {
            cJSON_AddNullToObject(o, "hub");
            cJSON_AddNullToObject(o, "port");
        }
    }
    const hub_ctl_port_t *port = is_virtual ? NULL : find_port(snap, n_hubs, d->busid);
    if (port) {
        cJSON_AddStringToObject(o, "port_power_status", port->powered ? "on" : "off");
        cJSON_AddStringToObject(o, "port_connect_status", port->connected ? "connected" : "disconnected");
    } else {
        cJSON_AddNullToObject(o, "port_power_status");
        cJSON_AddNullToObject(o, "port_connect_status");
    }
    return o;
}

static api_result_t core_devices(void)
{
    size_t devs_max = CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX;
    usbip_backend_device_t *devs = malloc(devs_max * sizeof(*devs));
    if (devs == NULL) {
        return fail(503, "out of memory");
    }
    size_t n = usb_backend_get_devices(devs, devs_max);
    size_t n_hubs = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n_hubs);

    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "devices");
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, device_json(&devs[i], snap, n_hubs));
    }
    free(snap);
    free(devs);
    return result(200, j);
}

static api_result_t core_device(const char *busid)
{
    char key[32] = {0};
    strlcpy(key, busid, sizeof(key));
    usbip_backend_device_t *dev = malloc(sizeof(*dev));
    if (dev == NULL) {
        return fail(503, "out of memory");
    }
    if (!usb_backend_get_device_by_busid(key, dev)) {
        free(dev);
        return fail(404, "no device with that busid");
    }
    size_t n_hubs = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n_hubs);
    cJSON *j = device_json(dev, snap, n_hubs);
    free(snap);
    free(dev);
    return result(200, j);
}

static cJSON *port_json(const hub_ctl_port_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "port", p->port);
    cJSON_AddStringToObject(o, "path", p->path);
    cJSON_AddStringToObject(o, "power", p->powered ? "on" : "off");
    cJSON_AddBoolToObject(o, "connected", p->connected);
    cJSON_AddBoolToObject(o, "enabled", p->enabled);
    cJSON_AddBoolToObject(o, "suspended", p->suspended);
    cJSON_AddBoolToObject(o, "over_current", p->over_current);
    cJSON_AddBoolToObject(o, "user_off", p->user_off);
    add_str_or_null(o, "speed", p->speed);
    if (p->has_device || p->has_hub) {
        cJSON *d = cJSON_AddObjectToObject(o, "device");
        cJSON_AddStringToObject(d, "type", p->has_hub ? "hub" : "device");
        cJSON_AddStringToObject(d, "busid", p->path);
        add_hex(d, "vid", p->id_vendor, 4);
        add_hex(d, "pid", p->id_product, 4);
    } else {
        cJSON_AddNullToObject(o, "device");
    }
    return o;
}

static api_result_t core_hubs(void)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "hubs");
    for (size_t h = 0; h < n; h++) {
        const hub_ctl_hub_status_t *s = &snap[h];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", s->hub.path);
        cJSON_AddNumberToObject(o, "address", s->hub.addr);
        add_hex(o, "vid", s->hub.id_vendor, 4);
        add_hex(o, "pid", s->hub.id_product, 4);
        add_str_or_null(o, "manufacturer", s->hub.manufacturer);
        add_str_or_null(o, "product", s->hub.product);
        cJSON_AddBoolToObject(o, "ready", s->info_ok);
        if (s->info_ok) {
            cJSON_AddNumberToObject(o, "num_ports", s->info.num_ports);
            cJSON_AddStringToObject(o, "power_switching", hub_ctl_power_switching_name(s->info.power_switching));
            cJSON_AddNumberToObject(o, "pwr_on_to_pwr_good_ms", s->info.pwr_on_to_pwr_good_ms);
            cJSON_AddBoolToObject(o, "compound", s->info.compound);
        }
        cJSON *ports = cJSON_AddArrayToObject(o, "ports");
        for (uint8_t p = 0; p < s->num_ports; p++) {
            cJSON_AddItemToArray(ports, port_json(&s->ports[p]));
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(snap);
    return result(200, j);
}

static api_result_t core_ports(void)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(j, "ports");
    for (size_t h = 0; h < n; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            cJSON *pj = port_json(&snap[h].ports[p]);
            cJSON_AddStringToObject(pj, "power_switching",
                                    hub_ctl_power_switching_name(snap[h].info.power_switching));
            cJSON_AddItemToArray(arr, pj);
        }
    }
    free(snap);
    return result(200, j);
}

static api_result_t core_port_get(const char *path)
{
    size_t n = 0;
    hub_ctl_hub_status_t *snap = snapshot_hubs(&n);
    if (snap == NULL) {
        return fail(503, "out of memory");
    }
    const hub_ctl_port_t *p = find_port(snap, n, path);
    api_result_t r;
    if (p == NULL) {
        r = fail(404, "no such hub port");
    } else {
        cJSON *j = port_json(p);
        cJSON_AddBoolToObject(j, "ok", true);
        r = result(200, j);
    }
    free(snap);
    return r;
}

static api_result_t port_action_ok(const char *path, const char *action)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "port", path);
    cJSON_AddStringToObject(j, "action", action);
    return result(200, j);
}

static api_result_t core_port_power(const char *path, bool on, bool force)
{
    char msg[200] = "";
    esp_err_t err = hub_ctl_port_power(path, on, force, msg, sizeof(msg));
    if (err != ESP_OK) {
        return fail(status_for_err(err), msg[0] ? msg : esp_err_to_name(err));
    }
    return port_action_ok(path, on ? "on" : "off");
}

static api_result_t core_port_cycle(const char *path, int off_ms, bool force)
{
    if (off_ms < 0) {
        return fail(400, "off_ms must be >= 0");
    }
    char msg[200] = "";
    esp_err_t err = hub_ctl_port_cycle(path, (uint32_t)off_ms, force, msg, sizeof(msg));
    if (err != ESP_OK) {
        return fail(status_for_err(err), msg[0] ? msg : esp_err_to_name(err));
    }
    api_result_t r = port_action_ok(path, "cycle");
    cJSON_AddNumberToObject(r.json, "off_ms", off_ms);
    cJSON_AddBoolToObject(r.json, "async", true);
    return r;
}

static api_result_t core_ports_all(bool on)
{
    int n = hub_ctl_all_ports(on);
    if (n < 0) {
        return fail(503, "out of memory");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "action", on ? "all_on" : "all_off");
    cJSON_AddNumberToObject(j, "ports", n);
    return result(200, j);
}

static api_result_t core_set_device_name(const char *busid, const char *name)
{
    if (busid == NULL || strlen(busid) >= 32) {
        return fail(400, "bad busid");
    }
    esp_err_t err = device_naming_set_device_name(busid, name ? name : "");
    if (err != ESP_OK) {
        return fail(500, esp_err_to_name(err));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "busid", busid);
    cJSON_AddStringToObject(j, "name", name ? name : "");
    return result(200, j);
}

/* --- analog mux (response shapes follow mux_api/server.py) --- */

static api_result_t core_mux_status(void)
{
    return result(200, analog_mux_describe());
}

static api_result_t core_mux_duts(void)
{
    cJSON *full = analog_mux_describe();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "active", cJSON_DetachItemFromObject(full, "active"));
    cJSON_AddItemToObject(j, "duts", cJSON_DetachItemFromObject(full, "duts"));
    cJSON_Delete(full);
    return result(200, j);
}

static api_result_t active_result(void)
{
    char active[64];
    analog_mux_get_active(active, sizeof(active));
    cJSON *j = cJSON_CreateObject();
    add_str_or_null(j, "active", active);
    return result(200, j);
}

static api_result_t core_mux_select_dut(const char *dut)
{
    char err[160] = "";
    esp_err_t e = analog_mux_select_dut(dut, err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "error", "unknown dut");
        cJSON_AddStringToObject(j, "dut", dut);
        cJSON_AddItemToObject(j, "duts", analog_mux_dut_names());
        return result(404, j);
    }
    if (e != ESP_OK) {
        return mux_fail(503, err);
    }
    return active_result();
}

static api_result_t unknown_group(const char *group)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", "unknown group");
    cJSON_AddStringToObject(j, "group", group);
    cJSON_AddItemToObject(j, "groups", analog_mux_group_names());
    return result(404, j);
}

static api_result_t core_mux_select_channel(const char *group, int channel)
{
    char err[160] = "";
    char active[64] = "";
    esp_err_t e = analog_mux_select_channel(group, channel, active, sizeof(active), err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        return unknown_group(group);
    }
    if (e != ESP_OK) {
        return mux_fail(e == ESP_ERR_INVALID_ARG ? 400 : 503, err);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "active", active);
    cJSON_AddStringToObject(j, "group", group);
    cJSON_AddNumberToObject(j, "channel", channel);
    return result(200, j);
}

static api_result_t core_mux_set_channel(const char *group, int channel, bool closed)
{
    char err[160] = "";
    esp_err_t e = analog_mux_set_channel(group, channel, closed, err, sizeof(err));
    if (e == ESP_ERR_NOT_FOUND) {
        return unknown_group(group);
    }
    if (e != ESP_OK) {
        return mux_fail(e == ESP_ERR_INVALID_ARG ? 400 : 503, err);
    }
    return result(200, analog_mux_describe());
}

static api_result_t core_mux_isolate(void)
{
    char err[160] = "";
    if (analog_mux_isolate(err, sizeof(err)) != ESP_OK) {
        return mux_fail(503, err);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNullToObject(j, "active");
    return result(200, j);
}

static api_result_t core_mux_topology_get(void)
{
    return result(200, analog_mux_get_topology());
}

static api_result_t core_mux_topology_set(const cJSON *topology)
{
    if (!cJSON_IsObject(topology)) {
        return mux_fail(400, "expected a topology JSON object");
    }
    char err[200] = "";
    bool saved = false;
    esp_err_t e = analog_mux_apply_topology(topology, &saved, err, sizeof(err));
    if (e != ESP_OK) {
        char msg[240];
        snprintf(msg, sizeof(msg), "invalid topology: %s", err);
        return mux_fail(400, msg);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "applied", true);
    cJSON_AddBoolToObject(j, "saved", saved);
    if (saved) {
        cJSON_AddNullToObject(j, "note");
    } else {
        cJSON_AddStringToObject(j, "note", "applied but not saved: NVS write failed");
    }
    char active[64];
    analog_mux_get_active(active, sizeof(active));
    add_str_or_null(j, "active", active);
    return result(200, j);
}

static api_result_t core_mux_probe(void)
{
    cJSON *j = analog_mux_probe();
    if (j == NULL) {
        return mux_fail(400, "no control bus available");
    }
    return result(200, j);
}

/* ===========================================================================
 *  Schema
 * ======================================================================== */

typedef struct {
    const char *method;
    const char *path;
    bool auth;
    const char *desc;
    const char *body;
} api_route_doc_t;

static const api_route_doc_t k_routes[] = {
    { "GET",  "/ping",                              false, "Liveness check", NULL },
    { "GET",  "/api/info",                          false, "Bridge identity, firmware version, auth and mux bus state", NULL },
    { "GET",  "/api/schema",                        false, "This endpoint list", NULL },
    { "GET",  "/api/usb/devices",                   false, "Exported USB devices with descriptors, topology and port power state. busid is the Linux style port path used by usbip", NULL },
    { "GET",  "/api/usb/devices/{busid}",           false, "One device; 404 when absent (presence check)", NULL },
    { "POST", "/api/devices/{busid}/name",          true,  "Set a friendly name for the device on a port", "{\"name\":str}" },
    { "GET",  "/api/usb/hubs",                      false, "Hubs with power switching mode and per-port status", NULL },
    { "POST", "/api/usb/debug",                     false, "Log the USB host's hub, port and enumeration state to the console (diagnostics)", NULL },
    { "GET",  "/api/ports",                         false, "Every downstream hub port (alias /ports)", NULL },
    { "GET",  "/api/ports/{port}",                  false, "One port, e.g. 1-1.3", NULL },
    { "POST", "/api/ports/{port}/on",               true,  "Power a hub port on (SetPortFeature PORT_POWER)", "{\"force\":bool}" },
    { "POST", "/api/ports/{port}/off",              true,  "Power a hub port off. Refused for ganged/non-switching hubs and ports leading to hubs unless force", "{\"force\":bool}" },
    { "POST", "/api/ports/{port}/cycle",            true,  "Power off, wait off_ms (default 1000), power on; runs in the background", "{\"off_ms\":int,\"force\":bool}" },
    { "POST", "/api/ports/off",                     true,  "Power off every per-port switched port that does not lead to a hub", NULL },
    { "POST", "/api/ports/on",                      true,  "Power on every per-port switched port", NULL },
    { "GET",  "/api/status",                        false, "Analog mux state {active, manual, duts, groups}", NULL },
    { "GET",  "/api/duts",                          false, "Analog mux DUTs {active, duts}", NULL },
    { "POST", "/api/select",                        true,  "Connect one DUT to the shared I2C chain (break-before-make)", "{\"dut\":str}" },
    { "POST", "/api/select/{dut}",                  true,  "Same as /api/select (GET also accepted)", NULL },
    { "POST", "/api/isolate",                       true,  "Open every mux channel", NULL },
    { "POST", "/api/groups/{group}/select/{channel}", true, "Exclusively connect group+channel (break-before-make)", NULL },
    { "POST", "/api/groups/{group}/channel",        true,  "Manually open/close one route (marks the mux manual)", "{\"channel\":int,\"closed\":bool}" },
    { "GET",  "/api/topology",                      false, "Mux topology", NULL },
    { "PUT",  "/api/topology",                      true,  "Validate, apply and save a mux topology (POST also accepted)", "{\"groups\":[...],\"duts\":[...]}" },
    { "GET",  "/api/probe",                         false, "Scan the mux control bus", NULL },
    { "GET",  "/api/auth",                          false, "Whether a token is required for mutating requests", NULL },
    { "POST", "/api/auth/token",                    true,  "Set the API token (empty string clears it)", "{\"token\":str}" },
    { "POST", "/api/reboot",                        true,  "Restart the bridge", NULL },
    { "POST", "/mcp",                               true,  "Model Context Protocol endpoint (Streamable HTTP, JSON responses, stateless)", "JSON-RPC 2.0" },
};

static api_result_t core_schema(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", "esp-usbip-bridge");
    cJSON_AddStringToObject(j, "version", HARNESS_FW_VERSION);
    cJSON_AddStringToObject(j, "auth", "When a token is set, every non-GET request (and /api/select/*) needs Authorization: Bearer <token>");
    cJSON_AddStringToObject(j, "errors", "Failures return a 4xx/5xx status with {\"error\":msg} (and \"ok\":false on device and port endpoints)");
    cJSON *arr = cJSON_AddArrayToObject(j, "endpoints");
    for (size_t i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "method", k_routes[i].method);
        cJSON_AddStringToObject(o, "path", k_routes[i].path);
        cJSON_AddBoolToObject(o, "auth", k_routes[i].auth);
        cJSON_AddStringToObject(o, "description", k_routes[i].desc);
        if (k_routes[i].body) {
            cJSON_AddStringToObject(o, "body", k_routes[i].body);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return result(200, j);
}

/* ===========================================================================
 *  MCP (Streamable HTTP, stateless, JSON responses)
 * ======================================================================== */

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;
} mcp_tool_t;

#define NO_ARGS "{\"type\":\"object\",\"properties\":{}}"

static const mcp_tool_t k_tools[] = {
    { "get_bridge_info", "Bridge identity, firmware version, uptime and whether auth is required.", NO_ARGS },
    { "list_usb_devices",
      "List USB devices exported over USB/IP with VID/PID, strings, serial, speed, topology "
      "(hub and port) and the power state of the port they are on. busid is the Linux style port "
      "path to pass to `usbip attach -r <bridge> -b <busid>`.", NO_ARGS },
    { "get_usb_device", "Details of one exported USB device. Errors if no device is on that port.",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\",\"description\":\"Port path, e.g. 1-1.3\"}},\"required\":[\"busid\"]}" },
    { "list_hubs",
      "List USB hubs with their power switching mode (per-port, ganged or none) and the state of every "
      "port: power, connection, speed, over-current and what is attached.", NO_ARGS },
    { "set_port_power",
      "Switch a hub port's VBUS on or off (per-port power switching). Powering off disconnects the "
      "device like unplugging it. Refused for hubs without per-port switching, and when powering off a "
      "port that leads to another hub, unless force is true.",
      "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"string\",\"description\":\"Port path, e.g. 1-1.3\"},"
      "\"on\":{\"type\":\"boolean\"},\"force\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"port\",\"on\"]}" },
    { "power_cycle_port",
      "Power a hub port off, wait off_ms, and power it back on (runs in the background; poll "
      "list_usb_devices to see the device come back).",
      "{\"type\":\"object\",\"properties\":{\"port\":{\"type\":\"string\"},\"off_ms\":{\"type\":\"integer\",\"default\":1000},"
      "\"force\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"port\"]}" },
    { "set_device_name", "Give the device on a port a friendly name (stored per port path).",
      "{\"type\":\"object\",\"properties\":{\"busid\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"busid\",\"name\"]}" },
    { "mux_status",
      "Analog I2C mux state: which DUT is connected to the shared I2C chain, groups and channel states.", NO_ARGS },
    { "mux_select",
      "Connect exactly one DUT to the shared I2C chain, break-before-make. Give either dut (a name "
      "from the topology) or group and channel.",
      "{\"type\":\"object\",\"properties\":{\"dut\":{\"type\":\"string\"},\"group\":{\"type\":\"string\"},\"channel\":{\"type\":\"integer\"}}}" },
    { "mux_isolate", "Open every analog mux channel so no DUT is connected.", NO_ARGS },
    { "mux_set_channel",
      "Manually open or close one mux route without touching the others (bring-up only; can connect "
      "several DUTs at once).",
      "{\"type\":\"object\",\"properties\":{\"group\":{\"type\":\"string\"},\"channel\":{\"type\":\"integer\"},"
      "\"closed\":{\"type\":\"boolean\"}},\"required\":[\"group\",\"channel\",\"closed\"]}" },
    { "mux_get_topology", "Get the analog mux topology (groups of ADG729/ADG728 switches and DUT names).", NO_ARGS },
    { "mux_set_topology",
      "Validate, apply and save a new analog mux topology. Groups: {name, type: adg729 (address) or "
      "adg728_pair (sda_address, scl_address)}; duts: {name, group, channel}.",
      "{\"type\":\"object\",\"properties\":{\"topology\":{\"type\":\"object\"}},\"required\":[\"topology\"]}" },
};

static cJSON *jsonrpc_response(const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    return r;
}

static cJSON *jsonrpc_error(const cJSON *id, int code, const char *msg)
{
    cJSON *r = jsonrpc_response(id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return r;
}

static api_result_t mcp_call_tool(const char *name, const cJSON *args)
{
    if (strcmp(name, "get_bridge_info") == 0) {
        return core_info();
    }
    if (strcmp(name, "list_usb_devices") == 0) {
        return core_devices();
    }
    if (strcmp(name, "get_usb_device") == 0) {
        const char *busid = json_string(args, "busid");
        return busid ? core_device(busid) : fail(400, "busid is required");
    }
    if (strcmp(name, "list_hubs") == 0) {
        return core_hubs();
    }
    if (strcmp(name, "set_port_power") == 0) {
        const char *port = json_string(args, "port");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(args, "on");
        if (port == NULL || on == NULL) {
            return fail(400, "port and on are required");
        }
        return core_port_power(port, json_bool(args, "on", false), json_bool(args, "force", false));
    }
    if (strcmp(name, "power_cycle_port") == 0) {
        const char *port = json_string(args, "port");
        int off_ms = API_DEFAULT_OFF_MS;
        json_int(args, "off_ms", &off_ms);
        return port ? core_port_cycle(port, off_ms, json_bool(args, "force", false))
                    : fail(400, "port is required");
    }
    if (strcmp(name, "set_device_name") == 0) {
        return core_set_device_name(json_string(args, "busid"), json_string(args, "name"));
    }
    if (strcmp(name, "mux_status") == 0) {
        return core_mux_status();
    }
    if (strcmp(name, "mux_select") == 0) {
        const char *dut = json_string(args, "dut");
        if (dut) {
            return core_mux_select_dut(dut);
        }
        const char *group = json_string(args, "group");
        int channel;
        if (group && json_int(args, "channel", &channel)) {
            return core_mux_select_channel(group, channel);
        }
        return mux_fail(400, "give dut, or group and channel");
    }
    if (strcmp(name, "mux_isolate") == 0) {
        return core_mux_isolate();
    }
    if (strcmp(name, "mux_set_channel") == 0) {
        const char *group = json_string(args, "group");
        int channel;
        if (group == NULL || !json_int(args, "channel", &channel) ||
                cJSON_GetObjectItemCaseSensitive(args, "closed") == NULL) {
            return mux_fail(400, "group, channel and closed are required");
        }
        return core_mux_set_channel(group, channel, json_bool(args, "closed", false));
    }
    if (strcmp(name, "mux_get_topology") == 0) {
        return core_mux_topology_get();
    }
    if (strcmp(name, "mux_set_topology") == 0) {
        return core_mux_topology_set(cJSON_GetObjectItemCaseSensitive(args, "topology"));
    }
    return result(-1, NULL);
}

/* Returns the JSON-RPC response, or NULL for a notification. */
static cJSON *mcp_dispatch(const cJSON *req)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const char *method = json_string(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if (method == NULL) {
        return jsonrpc_error(id, -32600, "Invalid Request");
    }
    if (id == NULL) {
        return NULL;    /* notification, e.g. notifications/initialized */
    }

    if (strcmp(method, "initialize") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        const char *requested = json_string(params, "protocolVersion");
        cJSON_AddStringToObject(res, "protocolVersion",
                                (requested && (strcmp(requested, "2025-06-18") == 0 ||
                                               strcmp(requested, "2025-03-26") == 0))
                                ? requested : MCP_PROTOCOL_VERSION);
        cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", "esp-usbip-bridge");
        cJSON_AddStringToObject(info, "version", HARNESS_FW_VERSION);
        cJSON_AddStringToObject(res, "instructions",
            "USB/IP hardware-in-the-loop bridge. USB devices behind its hubs are exported over USB/IP "
            "(TCP 3240); attach them from Linux with `usbip attach -r <bridge> -b <busid>`. Use list_hubs "
            "and set_port_power/power_cycle_port to switch DUT power per hub port, and the mux_* tools to "
            "route the shared I2C sensor chain to one DUT.");
        return r;
    }
    if (strcmp(method, "ping") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON_AddObjectToObject(r, "result");
        return r;
    }
    if (strcmp(method, "tools/list") == 0) {
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        cJSON *arr = cJSON_AddArrayToObject(res, "tools");
        for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", k_tools[i].name);
            cJSON_AddStringToObject(t, "description", k_tools[i].description);
            cJSON_AddItemToObject(t, "inputSchema", cJSON_Parse(k_tools[i].input_schema));
            cJSON_AddItemToArray(arr, t);
        }
        return r;
    }
    if (strcmp(method, "tools/call") == 0) {
        const char *name = json_string(params, "name");
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (name == NULL) {
            return jsonrpc_error(id, -32602, "params.name is required");
        }
        api_result_t tr = mcp_call_tool(name, args);
        if (tr.status < 0) {
            return jsonrpc_error(id, -32602, "Unknown tool");
        }
        cJSON *r = jsonrpc_response(id);
        cJSON *res = cJSON_AddObjectToObject(r, "result");
        char *text = tr.json ? cJSON_PrintUnformatted(tr.json) : NULL;
        cJSON *content = cJSON_AddArrayToObject(res, "content");
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "type", "text");
        cJSON_AddStringToObject(c, "text", text ? text : "{}");
        cJSON_AddItemToArray(content, c);
        if (text) {
            cJSON_free(text);
        }
        if (cJSON_IsObject(tr.json)) {
            cJSON_AddItemToObject(res, "structuredContent", tr.json);
        } else {
            cJSON_Delete(tr.json);
        }
        cJSON_AddBoolToObject(res, "isError", tr.status >= 400);
        return r;
    }
    return jsonrpc_error(id, -32601, "Method not found");
}

/* ===========================================================================
 *  REST routing
 * ======================================================================== */

static void respond_json(api_response_t *resp, api_result_t r)
{
    resp->status = r.status;
    resp->content_type = NULL;
    resp->body = NULL;
    resp->len = 0;
    if (r.json) {
        resp->body = cJSON_PrintUnformatted(r.json);
        resp->len = resp->body ? strlen(resp->body) : 0;
        cJSON_Delete(r.json);
    }
}

static void respond_empty(api_response_t *resp, int status)
{
    resp->status = status;
    resp->content_type = NULL;
    resp->body = NULL;
    resp->len = 0;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static api_result_t core_reboot(void)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        esp_timer_create_args_t args = { .callback = reboot_cb, .name = "api_reboot" };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return fail(503, "cannot schedule reboot");
        }
    }
    esp_timer_start_once(timer, API_REBOOT_DELAY_US);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "rebooting", true);
    return result(200, j);
}

static bool is_method(const char *method, const char *m)
{
    return strcmp(method, m) == 0;
}

static bool path_is(const char *path, const char *p)
{
    return strcmp(path, p) == 0;
}

bool controller_api_handle(const char *method, const char *url, const char *auth,
                           const char *body, size_t body_len, api_response_t *resp)
{
    /* Split path and query */
    char path[256];
    const char *q = strchr(url, '?');
    size_t plen = q ? (size_t)(q - url) : strlen(url);
    if (plen >= sizeof(path)) {
        return false;
    }
    memcpy(path, url, plen);
    path[plen] = '\0';
    const char *query = q ? q + 1 : NULL;

    /* Normalise the solenoid-style "/ports" alias to "/api/ports" */
    char aliased[264];
    if (strncmp(path, "/ports", 6) == 0 && (path[6] == '\0' || path[6] == '/')) {
        snprintf(aliased, sizeof(aliased), "/api%s", path);
        strlcpy(path, aliased, sizeof(path));
    }

    const bool known = path_is(path, "/ping") || path_is(path, "/mcp") ||
                       strncmp(path, "/api/info", 9) == 0 || path_is(path, "/api/schema") ||
                       strncmp(path, "/api/usb/", 9) == 0 || strncmp(path, "/api/ports", 10) == 0 ||
                       path_is(path, "/api/status") || path_is(path, "/api/duts") ||
                       strncmp(path, "/api/select", 11) == 0 || path_is(path, "/api/isolate") ||
                       strncmp(path, "/api/groups/", 12) == 0 || path_is(path, "/api/topology") ||
                       path_is(path, "/api/probe") || strncmp(path, "/api/auth", 9) == 0 ||
                       path_is(path, "/api/reboot");
    if (!known) {
        return false;
    }

    /* CORS preflight */
    if (is_method(method, "OPTIONS")) {
        respond_empty(resp, 204);
        return true;
    }

    const bool mutating = !(is_method(method, "GET") || is_method(method, "HEAD")) ||
                          strncmp(path, "/api/select", 11) == 0;
    if (mutating && !controller_api_authorized(auth)) {
        respond_json(resp, mux_fail(401, "unauthorized"));
        return true;
    }

    cJSON *json = (body != NULL && body_len > 0) ? cJSON_ParseWithLength(body, body_len) : NULL;
    api_result_t r = { .status = 0 };
    char seg[64];
    char seg2[64];

    if (path_is(path, "/ping")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        r = result(200, j);
    } else if (path_is(path, "/api/info")) {
        r = core_info();
    } else if (path_is(path, "/api/schema")) {
        r = core_schema();
    } else if (path_is(path, "/mcp")) {
        if (!is_method(method, "POST")) {
            r = mux_fail(405, "only POST is supported (stateless, no SSE stream)");
        } else if (!cJSON_IsObject(json)) {
            cJSON *e = jsonrpc_error(NULL, -32700, "Parse error");
            r = result(400, e);
        } else {
            cJSON *rpc = mcp_dispatch(json);
            if (rpc == NULL) {
                cJSON_Delete(json);
                respond_empty(resp, 202);
                return true;
            }
            r = result(200, rpc);
        }
    } else if (path_is(path, "/api/usb/devices")) {
        r = core_devices();
    } else if (strncmp(path, "/api/usb/devices/", 17) == 0) {
        take_segment(path + 17, seg, sizeof(seg));
        r = core_device(seg);
    } else if (path_is(path, "/api/usb/hubs")) {
        r = core_hubs();
    } else if (path_is(path, "/api/usb/debug")) {
        /* Diagnostics: the USB host's hub/port/enumeration state goes to the console */
        esp_err_t err = usb_host_hub_debug_dump();
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", err == ESP_OK);
        cJSON_AddStringToObject(j, "note", "state logged to the console (UART0 and USB Serial/JTAG)");
        r = result(err == ESP_OK ? 200 : 503, j);
    } else if (path_is(path, "/api/ports")) {
        r = core_ports();
    } else if (path_is(path, "/api/ports/off") && is_method(method, "POST")) {
        r = core_ports_all(false);
    } else if (path_is(path, "/api/ports/on") && is_method(method, "POST")) {
        r = core_ports_all(true);
    } else if (strncmp(path, "/api/ports/", 11) == 0) {
        const char *rest = take_segment(path + 11, seg, sizeof(seg));
        bool force = json_bool(json, "force", query_flag(query, "force"));
        if (*rest == '\0') {
            r = is_method(method, "GET") ? core_port_get(seg) : fail(405, "use GET");
        } else if (!is_method(method, "POST")) {
            r = fail(405, "use POST");
        } else if (strcmp(rest, "/on") == 0) {
            r = core_port_power(seg, true, force);
        } else if (strcmp(rest, "/off") == 0) {
            r = core_port_power(seg, false, force);
        } else if (strcmp(rest, "/cycle") == 0) {
            int off_ms = query_int(query, "off_ms", API_DEFAULT_OFF_MS);
            json_int(json, "off_ms", &off_ms);
            r = core_port_cycle(seg, off_ms, force);
        } else {
            r = fail(404, "unknown port action (on, off, cycle)");
        }
    } else if (path_is(path, "/api/status")) {
        r = core_mux_status();
    } else if (path_is(path, "/api/duts")) {
        r = core_mux_duts();
    } else if (path_is(path, "/api/probe")) {
        r = core_mux_probe();
    } else if (path_is(path, "/api/select")) {
        const char *dut = json_string(json, "dut");
        r = (is_method(method, "POST") && dut) ? core_mux_select_dut(dut)
            : mux_fail(400, "expected JSON body {\"dut\": \"<name>\"}");
    } else if (strncmp(path, "/api/select/", 12) == 0) {
        take_segment(path + 12, seg, sizeof(seg));
        r = core_mux_select_dut(seg);
    } else if (path_is(path, "/api/isolate")) {
        r = is_method(method, "POST") ? core_mux_isolate() : mux_fail(405, "use POST");
    } else if (strncmp(path, "/api/groups/", 12) == 0) {
        const char *rest = take_segment(path + 12, seg, sizeof(seg));
        if (strncmp(rest, "/select/", 8) == 0 && is_method(method, "POST")) {
            take_segment(rest + 8, seg2, sizeof(seg2));
            char *end = NULL;
            long ch = strtol(seg2, &end, 10);
            r = (end != seg2 && *end == '\0') ? core_mux_select_channel(seg, (int)ch)
                : mux_fail(400, "channel must be an integer");
        } else if (strcmp(rest, "/channel") == 0 && is_method(method, "POST")) {
            int ch;
            if (!json_int(json, "channel", &ch) || cJSON_GetObjectItemCaseSensitive(json, "closed") == NULL) {
                r = mux_fail(400, "expected JSON body {\"channel\": <int>, \"closed\": <bool>}");
            } else {
                r = core_mux_set_channel(seg, ch, json_bool(json, "closed", false));
            }
        } else {
            r = mux_fail(404, "Not Found");
        }
    } else if (path_is(path, "/api/topology")) {
        if (is_method(method, "GET")) {
            r = core_mux_topology_get();
        } else if (is_method(method, "PUT") || is_method(method, "POST")) {
            r = core_mux_topology_set(json);
        } else {
            r = mux_fail(405, "use GET or PUT");
        }
    } else if (path_is(path, "/api/auth")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
        r = result(200, j);
    } else if (path_is(path, "/api/auth/token") && is_method(method, "POST")) {
        const char *token = json_string(json, "token");
        if (token == NULL) {
            r = fail(400, "expected JSON body {\"token\": \"<token>\"} (empty clears)");
        } else {
            esp_err_t err = set_token(token);
            if (err != ESP_OK) {
                r = fail(status_for_err(err), esp_err_to_name(err));
            } else {
                cJSON *j = cJSON_CreateObject();
                cJSON_AddBoolToObject(j, "ok", true);
                cJSON_AddBoolToObject(j, "auth_required", s_token[0] != '\0');
                r = result(200, j);
            }
        }
    } else if (path_is(path, "/api/reboot") && is_method(method, "POST")) {
        r = core_reboot();
    } else {
        r = mux_fail(404, "Not Found");
    }

    cJSON_Delete(json);
    respond_json(resp, r);
    return true;
}
