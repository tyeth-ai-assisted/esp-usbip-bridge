#include "analog_mux.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "analog_mux";

#define MUX_MAX_GROUPS      8
#define MUX_MAX_DUTS        64
#define MUX_NAME_LEN        32
#define MUX_MAX_I2C_DEVS    (MUX_MAX_GROUPS * 2)
#define MUX_I2C_TIMEOUT_MS  50

#define MUX_NVS_NAMESPACE   "mux"
#define MUX_NVS_KEY         "topology"

/* Default 7-bit base addresses (A0/A1 = 0), as in switches.py */
#define ADG728_BASE_ADDRESS 0x4C
#define ADG729_BASE_ADDRESS 0x44

typedef enum {
    MUX_GROUP_ADG729,       /* dual 4:1, bank A = SDA (bits 0-3), bank B = SCL (bits 4-7) */
    MUX_GROUP_ADG728_PAIR,  /* two 8:1, one for SDA and one for SCL, same mask */
} mux_group_type_t;

typedef struct {
    char name[MUX_NAME_LEN];
    mux_group_type_t type;
    uint8_t addr[2];        /* ADG729: addr[0]; pair: SDA, SCL */
    uint8_t n_addr;
    uint8_t routes;
    uint8_t closed;         /* bit per route */
} mux_group_t;

typedef struct {
    char name[MUX_NAME_LEN];
    uint8_t group;          /* index into groups */
    uint8_t channel;
} mux_dut_t;

typedef struct {
    mux_group_t groups[MUX_MAX_GROUPS];
    size_t n_groups;
    mux_dut_t duts[MUX_MAX_DUTS];
    size_t n_duts;
} mux_map_t;

static struct {
    SemaphoreHandle_t lock;
    mux_map_t map;
    cJSON *topology;
    char active[MUX_NAME_LEN + 8];
    bool manual;
    i2c_master_bus_handle_t bus;
    struct {
        uint8_t addr;
        i2c_master_dev_handle_t dev;
    } devs[MUX_MAX_I2C_DEVS];
    size_t n_devs;
} s_mux;

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------------------
 *  Control bus
 * ------------------------------------------------------------------------- */

static void bus_init(void)
{
#if CONFIG_USBIP_MUX_I2C_SDA_GPIO >= 0 && CONFIG_USBIP_MUX_I2C_SCL_GPIO >= 0
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_USBIP_MUX_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_USBIP_MUX_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_mux.bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Control bus init failed (SDA=%d SCL=%d): %s; switching disabled",
                 CONFIG_USBIP_MUX_I2C_SDA_GPIO, CONFIG_USBIP_MUX_I2C_SCL_GPIO, esp_err_to_name(err));
        s_mux.bus = NULL;
        return;
    }
    ESP_LOGI(TAG, "Control bus on SDA=%d SCL=%d", CONFIG_USBIP_MUX_I2C_SDA_GPIO, CONFIG_USBIP_MUX_I2C_SCL_GPIO);
#else
    ESP_LOGI(TAG, "No control bus pins configured; switching disabled");
#endif
}

static i2c_master_dev_handle_t bus_dev(uint8_t addr)
{
    for (size_t i = 0; i < s_mux.n_devs; i++) {
        if (s_mux.devs[i].addr == addr) {
            return s_mux.devs[i].dev;
        }
    }
    if (s_mux.n_devs >= MUX_MAX_I2C_DEVS) {
        return NULL;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = CONFIG_USBIP_MUX_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_mux.bus, &dcfg, &dev) != ESP_OK) {
        return NULL;
    }
    s_mux.devs[s_mux.n_devs].addr = addr;
    s_mux.devs[s_mux.n_devs].dev = dev;
    s_mux.n_devs++;
    return dev;
}

static esp_err_t write_byte(uint8_t addr, uint8_t value, char *err, size_t err_len)
{
    if (s_mux.bus == NULL) {
        set_err(err, err_len, "control bus unavailable (I2C not initialised)");
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_dev_handle_t dev = bus_dev(addr);
    if (dev == NULL) {
        set_err(err, err_len, "cannot add I2C device 0x%02x", addr);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_master_transmit(dev, &value, 1, MUX_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        set_err(err, err_len, "write to 0x%02x failed: %s", addr, esp_err_to_name(ret));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* Push a group's closed state to its chip(s). */
static esp_err_t group_apply(const mux_group_t *g, char *err, size_t err_len)
{
    if (g->type == MUX_GROUP_ADG729) {
        uint8_t mask = 0;
        for (uint8_t ch = 0; ch < g->routes; ch++) {
            if (g->closed & (1U << ch)) {
                mask |= (1U << ch) | (1U << (ch + 4));  /* SDA bank A, SCL bank B */
            }
        }
        return write_byte(g->addr[0], mask, err, err_len);
    }
    esp_err_t ret = write_byte(g->addr[0], g->closed, err, err_len);  /* SDA lines */
    if (ret != ESP_OK) {
        return ret;
    }
    return write_byte(g->addr[1], g->closed, err, err_len);            /* SCL lines */
}

/* ---------------------------------------------------------------------------
 *  Topology parsing (config.py build_groups/build_duts + validate_map)
 * ------------------------------------------------------------------------- */

static bool parse_address(const cJSON *item, uint8_t *out)
{
    long v;
    if (cJSON_IsNumber(item)) {
        v = (long)item->valuedouble;
    } else if (cJSON_IsString(item)) {
        char *end = NULL;
        v = strtol(item->valuestring, &end, 0);   /* "0x44" or "68" */
        if (end == item->valuestring) {
            return false;
        }
        while (*end == ' ') {
            end++;
        }
        if (*end != '\0') {
            return false;
        }
    } else {
        return false;
    }
    if (v < 0 || v > 0x7F) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int find_group(const mux_map_t *map, const char *name)
{
    for (size_t i = 0; i < map->n_groups; i++) {
        if (strcmp(map->groups[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_dut(const mux_map_t *map, const char *name)
{
    for (size_t i = 0; i < map->n_duts; i++) {
        if (strcmp(map->duts[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t build_map(const cJSON *topology, mux_map_t *map, char *err, size_t err_len)
{
    memset(map, 0, sizeof(*map));
    if (!cJSON_IsObject(topology)) {
        set_err(err, err_len, "expected a topology JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(topology, "groups");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, groups) {
        const char *name = json_str(entry, "name");
        const char *type = json_str(entry, "type");
        if (name == NULL || type == NULL) {
            set_err(err, err_len, "group entries need \"name\" and \"type\"");
            return ESP_ERR_INVALID_ARG;
        }
        if (strlen(name) >= MUX_NAME_LEN) {
            set_err(err, err_len, "group name too long: '%s'", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (find_group(map, name) >= 0) {
            set_err(err, err_len, "duplicate group name: '%s'", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (map->n_groups >= MUX_MAX_GROUPS) {
            set_err(err, err_len, "too many groups (max %d)", MUX_MAX_GROUPS);
            return ESP_ERR_INVALID_ARG;
        }
        mux_group_t *g = &map->groups[map->n_groups];
        strlcpy(g->name, name, sizeof(g->name));
        if (strcasecmp(type, "adg729") == 0 || strcasecmp(type, "dual4") == 0) {
            g->type = MUX_GROUP_ADG729;
            g->routes = 4;
            g->n_addr = 1;
            g->addr[0] = ADG729_BASE_ADDRESS;
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(entry, "address");
            if (a != NULL && !parse_address(a, &g->addr[0])) {
                set_err(err, err_len, "group '%s': bad address", name);
                return ESP_ERR_INVALID_ARG;
            }
        } else if (strcasecmp(type, "adg728_pair") == 0 || strcasecmp(type, "adg728pair") == 0 ||
                   strcasecmp(type, "pair8") == 0) {
            g->type = MUX_GROUP_ADG728_PAIR;
            g->routes = 8;
            g->n_addr = 2;
            if (!parse_address(cJSON_GetObjectItemCaseSensitive(entry, "sda_address"), &g->addr[0]) ||
                    !parse_address(cJSON_GetObjectItemCaseSensitive(entry, "scl_address"), &g->addr[1])) {
                set_err(err, err_len, "group '%s': needs sda_address and scl_address", name);
                return ESP_ERR_INVALID_ARG;
            }
        } else {
            set_err(err, err_len, "unknown group type: '%s'", type);
            return ESP_ERR_INVALID_ARG;
        }
        map->n_groups++;
    }

    const cJSON *duts = cJSON_GetObjectItemCaseSensitive(topology, "duts");
    cJSON_ArrayForEach(entry, duts) {
        const char *name = json_str(entry, "name");
        const char *group = json_str(entry, "group");
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(entry, "channel");
        if (name == NULL || group == NULL || !(cJSON_IsNumber(ch) || cJSON_IsString(ch))) {
            set_err(err, err_len, "dut entries need \"name\", \"group\" and \"channel\"");
            return ESP_ERR_INVALID_ARG;
        }
        if (strlen(name) >= MUX_NAME_LEN) {
            set_err(err, err_len, "dut name too long: '%s'", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (find_dut(map, name) >= 0) {
            set_err(err, err_len, "duplicate dut name: '%s'", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (map->n_duts >= MUX_MAX_DUTS) {
            set_err(err, err_len, "too many duts (max %d)", MUX_MAX_DUTS);
            return ESP_ERR_INVALID_ARG;
        }
        int gi = find_group(map, group);
        if (gi < 0) {
            set_err(err, err_len, "dut '%s' references unknown group '%s'", name, group);
            return ESP_ERR_INVALID_ARG;
        }
        long channel = cJSON_IsNumber(ch) ? (long)ch->valuedouble : strtol(ch->valuestring, NULL, 10);
        if (channel < 0 || channel >= map->groups[gi].routes) {
            set_err(err, err_len, "dut '%s' channel %ld out of range for group '%s' (0..%d)",
                    name, channel, group, map->groups[gi].routes - 1);
            return ESP_ERR_INVALID_ARG;
        }
        mux_dut_t *d = &map->duts[map->n_duts++];
        strlcpy(d->name, name, sizeof(d->name));
        d->group = (uint8_t)gi;
        d->channel = (uint8_t)channel;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 *  Router (router.py)
 * ------------------------------------------------------------------------- */

static esp_err_t isolate_locked(char *err, size_t err_len)
{
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < s_mux.map.n_groups; i++) {
        s_mux.map.groups[i].closed = 0;
        esp_err_t r = group_apply(&s_mux.map.groups[i], err, err_len);
        if (r != ESP_OK && ret == ESP_OK) {
            ret = r;
        }
    }
    s_mux.active[0] = '\0';
    s_mux.manual = false;
    return ret;
}

static const char *channel_dut(size_t group, uint8_t channel)
{
    for (size_t i = 0; i < s_mux.map.n_duts; i++) {
        if (s_mux.map.duts[i].group == group && s_mux.map.duts[i].channel == channel) {
            return s_mux.map.duts[i].name;
        }
    }
    return NULL;
}

esp_err_t analog_mux_select_dut(const char *dut, char *err, size_t err_len)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    int di = find_dut(&s_mux.map, dut);
    if (di < 0) {
        xSemaphoreGive(s_mux.lock);
        set_err(err, err_len, "unknown dut");
        return ESP_ERR_NOT_FOUND;
    }
    mux_dut_t *d = &s_mux.map.duts[di];
    esp_err_t ret = isolate_locked(err, err_len);     /* break-before-make */
    if (ret == ESP_OK) {
        mux_group_t *g = &s_mux.map.groups[d->group];
        g->closed = 1U << d->channel;
        ret = group_apply(g, err, err_len);
    }
    if (ret == ESP_OK) {
        strlcpy(s_mux.active, d->name, sizeof(s_mux.active));
        s_mux.manual = false;
        ESP_LOGI(TAG, "Selected %s", d->name);
    }
    xSemaphoreGive(s_mux.lock);
    return ret;
}

esp_err_t analog_mux_select_channel(const char *group, int channel,
                                    char *active, size_t active_len,
                                    char *err, size_t err_len)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    int gi = find_group(&s_mux.map, group);
    if (gi < 0) {
        xSemaphoreGive(s_mux.lock);
        set_err(err, err_len, "unknown group");
        return ESP_ERR_NOT_FOUND;
    }
    mux_group_t *g = &s_mux.map.groups[gi];
    if (channel < 0 || channel >= g->routes) {
        xSemaphoreGive(s_mux.lock);
        set_err(err, err_len, "channel %d out of range for '%s' (0..%d)", channel, group, g->routes - 1);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = isolate_locked(err, err_len);     /* break-before-make across all groups */
    if (ret == ESP_OK) {
        g->closed = 1U << channel;                    /* exclusive within the group */
        ret = group_apply(g, err, err_len);
    }
    if (ret == ESP_OK) {
        const char *dut = channel_dut(gi, (uint8_t)channel);
        if (dut != NULL) {
            strlcpy(s_mux.active, dut, sizeof(s_mux.active));
        } else {
            snprintf(s_mux.active, sizeof(s_mux.active), "%s:ch%d", group, channel);
        }
        s_mux.manual = false;
        if (active != NULL) {
            strlcpy(active, s_mux.active, active_len);
        }
        ESP_LOGI(TAG, "Selected %s:ch%d (%s)", group, channel, s_mux.active);
    }
    xSemaphoreGive(s_mux.lock);
    return ret;
}

esp_err_t analog_mux_set_channel(const char *group, int channel, bool closed,
                                 char *err, size_t err_len)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    int gi = find_group(&s_mux.map, group);
    if (gi < 0) {
        xSemaphoreGive(s_mux.lock);
        set_err(err, err_len, "unknown group");
        return ESP_ERR_NOT_FOUND;
    }
    mux_group_t *g = &s_mux.map.groups[gi];
    if (channel < 0 || channel >= g->routes) {
        xSemaphoreGive(s_mux.lock);
        set_err(err, err_len, "channel %d out of range for '%s' (0..%d)", channel, group, g->routes - 1);
        return ESP_ERR_INVALID_ARG;
    }
    if (closed) {
        g->closed |= 1U << channel;
    } else {
        g->closed &= ~(1U << channel);
    }
    esp_err_t ret = group_apply(g, err, err_len);
    s_mux.active[0] = '\0';
    s_mux.manual = true;
    xSemaphoreGive(s_mux.lock);
    return ret;
}

esp_err_t analog_mux_isolate(char *err, size_t err_len)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    esp_err_t ret = isolate_locked(err, err_len);
    xSemaphoreGive(s_mux.lock);
    return ret;
}

void analog_mux_get_active(char *out, size_t out_len)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    strlcpy(out, s_mux.active, out_len);
    xSemaphoreGive(s_mux.lock);
}

bool analog_mux_bus_ok(void)
{
    return s_mux.bus != NULL;
}

/* ---------------------------------------------------------------------------
 *  Serialisation
 * ------------------------------------------------------------------------- */

static int cmp_dut_idx(const void *a, const void *b)
{
    return strcmp(s_mux.map.duts[*(const uint8_t *)a].name, s_mux.map.duts[*(const uint8_t *)b].name);
}

static int cmp_group_idx(const void *a, const void *b)
{
    return strcmp(s_mux.map.groups[*(const uint8_t *)a].name, s_mux.map.groups[*(const uint8_t *)b].name);
}

static void add_active(cJSON *obj)
{
    if (s_mux.active[0] != '\0') {
        cJSON_AddStringToObject(obj, "active", s_mux.active);
    } else {
        cJSON_AddNullToObject(obj, "active");
    }
}

static cJSON *duts_json_locked(void)
{
    uint8_t order[MUX_MAX_DUTS];
    for (size_t i = 0; i < s_mux.map.n_duts; i++) {
        order[i] = (uint8_t)i;
    }
    qsort(order, s_mux.map.n_duts, 1, cmp_dut_idx);
    cJSON *arr = cJSON_CreateArray();
    for (size_t k = 0; k < s_mux.map.n_duts; k++) {
        const mux_dut_t *d = &s_mux.map.duts[order[k]];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddStringToObject(o, "group", s_mux.map.groups[d->group].name);
        cJSON_AddNumberToObject(o, "channel", d->channel);
        cJSON_AddBoolToObject(o, "active", strcmp(d->name, s_mux.active) == 0);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *analog_mux_describe(void)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    cJSON *root = cJSON_CreateObject();
    add_active(root);
    cJSON_AddBoolToObject(root, "manual", s_mux.manual);
    cJSON_AddItemToObject(root, "duts", duts_json_locked());

    uint8_t order[MUX_MAX_GROUPS];
    for (size_t i = 0; i < s_mux.map.n_groups; i++) {
        order[i] = (uint8_t)i;
    }
    qsort(order, s_mux.map.n_groups, 1, cmp_group_idx);
    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    for (size_t k = 0; k < s_mux.map.n_groups; k++) {
        const mux_group_t *g = &s_mux.map.groups[order[k]];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", g->name);
        cJSON_AddNumberToObject(o, "routes", g->routes);
        cJSON *addrs = cJSON_AddArrayToObject(o, "addresses");
        for (uint8_t a = 0; a < g->n_addr; a++) {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02x", g->addr[a]);
            cJSON_AddItemToArray(addrs, cJSON_CreateString(hex));
        }
        cJSON *chans = cJSON_AddArrayToObject(o, "channels");
        for (uint8_t ch = 0; ch < g->routes; ch++) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddNumberToObject(c, "channel", ch);
            cJSON_AddBoolToObject(c, "closed", (g->closed >> ch) & 1U);
            const char *dut = channel_dut(order[k], ch);
            if (dut != NULL) {
                cJSON_AddStringToObject(c, "dut", dut);
            } else {
                cJSON_AddNullToObject(c, "dut");
            }
            cJSON_AddItemToArray(chans, c);
        }
        cJSON_AddItemToArray(groups, o);
    }
    xSemaphoreGive(s_mux.lock);
    return root;
}

cJSON *analog_mux_get_topology(void)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    cJSON *copy = cJSON_Duplicate(s_mux.topology, true);
    xSemaphoreGive(s_mux.lock);
    return copy;
}

cJSON *analog_mux_dut_names(void)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_mux.map.n_duts; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_mux.map.duts[i].name));
    }
    xSemaphoreGive(s_mux.lock);
    return arr;
}

cJSON *analog_mux_group_names(void)
{
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_mux.map.n_groups; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_mux.map.groups[i].name));
    }
    xSemaphoreGive(s_mux.lock);
    return arr;
}

cJSON *analog_mux_probe(void)
{
    if (s_mux.bus == NULL) {
        return NULL;
    }
    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    bool present[128] = {0};
    cJSON *root = cJSON_CreateObject();
    cJSON *scan = cJSON_AddArrayToObject(root, "scan");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_mux.bus, a, MUX_I2C_TIMEOUT_MS) == ESP_OK) {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02x", a);
            cJSON_AddItemToArray(scan, cJSON_CreateString(hex));
            present[a] = true;
        }
    }
    cJSON *groups = cJSON_AddArrayToObject(root, "groups");
    for (size_t i = 0; i < s_mux.map.n_groups; i++) {
        const mux_group_t *g = &s_mux.map.groups[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", g->name);
        cJSON *addrs = cJSON_AddArrayToObject(o, "addresses");
        bool all = true;
        for (uint8_t a = 0; a < g->n_addr; a++) {
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02x", g->addr[a]);
            cJSON_AddItemToArray(addrs, cJSON_CreateString(hex));
            all = all && present[g->addr[a]];
        }
        cJSON_AddBoolToObject(o, "present", all);
        cJSON_AddItemToArray(groups, o);
    }
    xSemaphoreGive(s_mux.lock);
    return root;
}

/* ---------------------------------------------------------------------------
 *  Topology persistence
 * ------------------------------------------------------------------------- */

static bool save_topology(const cJSON *topology)
{
    char *text = cJSON_PrintUnformatted(topology);
    if (text == NULL) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(MUX_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, MUX_NVS_KEY, text);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    cJSON_free(text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving topology failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static cJSON *load_topology(void)
{
    nvs_handle_t h;
    if (nvs_open(MUX_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    cJSON *topology = NULL;
    if (nvs_get_str(h, MUX_NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        char *text = malloc(len);
        if (text != NULL && nvs_get_str(h, MUX_NVS_KEY, text, &len) == ESP_OK) {
            topology = cJSON_Parse(text);
        }
        free(text);
    }
    nvs_close(h);
    return topology;
}

esp_err_t analog_mux_apply_topology(const cJSON *topology, bool *saved, char *err, size_t err_len)
{
    mux_map_t *map = malloc(sizeof(*map));
    if (map == NULL) {
        set_err(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = build_map(topology, map, err, err_len);    /* validate before touching hardware */
    if (ret != ESP_OK) {
        free(map);
        return ret;
    }
    cJSON *copy = cJSON_Duplicate(topology, true);

    xSemaphoreTake(s_mux.lock, portMAX_DELAY);
    isolate_locked(NULL, 0);                                    /* open current hardware */
    s_mux.map = *map;
    cJSON_Delete(s_mux.topology);
    s_mux.topology = copy;
    isolate_locked(NULL, 0);                                    /* open new hardware, active=None */
    xSemaphoreGive(s_mux.lock);
    free(map);

    bool ok = save_topology(topology);
    if (saved != NULL) {
        *saved = ok;
    }
    ESP_LOGI(TAG, "Topology applied (%u groups, %u duts)", (unsigned)s_mux.map.n_groups, (unsigned)s_mux.map.n_duts);
    return ESP_OK;
}

esp_err_t analog_mux_init(void)
{
    s_mux.lock = xSemaphoreCreateMutex();
    if (s_mux.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bus_init();

    cJSON *topology = load_topology();
    if (topology == NULL) {
        topology = cJSON_Parse("{\"groups\":[],\"duts\":[]}");
    }
    char err[128] = "";
    mux_map_t *map = malloc(sizeof(*map));
    if (map == NULL) {
        cJSON_Delete(topology);
        return ESP_ERR_NO_MEM;
    }
    if (build_map(topology, map, err, sizeof(err)) != ESP_OK) {
        ESP_LOGW(TAG, "Saved topology invalid (%s); starting empty", err);
        cJSON_Delete(topology);
        topology = cJSON_Parse("{\"groups\":[],\"duts\":[]}");
        memset(map, 0, sizeof(*map));
    }
    s_mux.map = *map;
    free(map);
    s_mux.topology = topology;

    if (isolate_locked(err, sizeof(err)) != ESP_OK && s_mux.map.n_groups > 0) {
        ESP_LOGW(TAG, "Isolating at boot failed: %s", err);
    }
    ESP_LOGI(TAG, "Analog mux ready (%u groups, %u duts)", (unsigned)s_mux.map.n_groups, (unsigned)s_mux.map.n_duts);
    return ESP_OK;
}
