#include "scpi_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

#include "analog_mux.h"
#include "hub_control.h"
#include "usb_backend.h"

/*
 * SCPI commands for the virtual test harness, appended to the esp-harness
 * table.  Queries that return structured data answer with one line of JSON.
 *
 *   HUB:LIST?                         JSON array of hubs and their ports
 *   HUB:PORT:POWer "1-1.3",ON|OFF[,FORCE]
 *   HUB:PORT:POWer? "1-1.3"           1 or 0
 *   HUB:PORT:CYCLe "1-1.3"[,<off_ms>[,FORCE]]
 *   USB:DEVices?                      JSON array of exported devices
 *   MUX:SELect "dut"
 *   MUX:SELect:CHANnel "group",<channel>
 *   MUX:ISOLate
 *   MUX:ACTive?                       "dut", or "" when isolated
 *   MUX:STATus?                       JSON mux state
 */

#define SCPI_TEXT_LEN 64

static bool param_text(scpi_t *ctx, char *buf, size_t size, bool mandatory)
{
    size_t len = 0;
    if (!SCPI_ParamCopyText(ctx, buf, size - 1, &len, mandatory)) {
        return false;
    }
    buf[len] = '\0';
    return true;
}

static scpi_result_t result_json(scpi_t *ctx, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (text == NULL) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_OUT_OF_MEMORY);
        return SCPI_RES_ERR;
    }
    SCPI_ResultCharacters(ctx, text, strlen(text));
    cJSON_free(text);
    return SCPI_RES_OK;
}

static scpi_result_t push_err(scpi_t *ctx, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        SCPI_ErrorPush(ctx, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        break;
    case ESP_ERR_NOT_FOUND:
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        break;
    case ESP_ERR_NOT_SUPPORTED:
    case ESP_ERR_INVALID_STATE:
        SCPI_ErrorPush(ctx, SCPI_ERROR_SETTINGS_CONFLICT);
        break;
    default:
        SCPI_ErrorPush(ctx, SCPI_ERROR_EXECUTION_ERROR);
        break;
    }
    return SCPI_RES_ERR;
}

static scpi_result_t hub_list_q(scpi_t *ctx)
{
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_OUT_OF_MEMORY);
        return SCPI_RES_ERR;
    }
    size_t n = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
    cJSON *arr = cJSON_CreateArray();
    for (size_t h = 0; h < n; h++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", snap[h].hub.path);
        cJSON_AddStringToObject(o, "power_switching",
                                snap[h].info_ok ? hub_ctl_power_switching_name(snap[h].info.power_switching) : "unknown");
        cJSON *ports = cJSON_AddArrayToObject(o, "ports");
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            const hub_ctl_port_t *pt = &snap[h].ports[p];
            cJSON *pj = cJSON_CreateObject();
            cJSON_AddStringToObject(pj, "path", pt->path);
            cJSON_AddBoolToObject(pj, "power", pt->powered);
            cJSON_AddBoolToObject(pj, "connected", pt->connected);
            cJSON_AddBoolToObject(pj, "device", pt->has_device || pt->has_hub);
            cJSON_AddItemToArray(ports, pj);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(snap);
    return result_json(ctx, arr);
}

static scpi_result_t hub_port_power(scpi_t *ctx)
{
    char path[32];
    scpi_bool_t on;
    if (!param_text(ctx, path, sizeof(path), true) || !SCPI_ParamBool(ctx, &on, true)) {
        return SCPI_RES_ERR;
    }
    char extra[16] = "";
    param_text(ctx, extra, sizeof(extra), false);
    bool force = strcasecmp(extra, "FORCE") == 0;
    esp_err_t err = hub_ctl_port_power(path, on, force, NULL, NULL, 0);
    return err == ESP_OK ? SCPI_RES_OK : push_err(ctx, err);
}

static scpi_result_t hub_port_power_q(scpi_t *ctx)
{
    char path[32];
    if (!param_text(ctx, path, sizeof(path), true)) {
        return SCPI_RES_ERR;
    }
    hub_ctl_hub_status_t *snap = malloc(USB_BACKEND_MAX_HUBS * sizeof(*snap));
    if (snap == NULL) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_OUT_OF_MEMORY);
        return SCPI_RES_ERR;
    }
    size_t n = hub_ctl_snapshot(snap, USB_BACKEND_MAX_HUBS);
    int state = -1;
    for (size_t h = 0; h < n && state < 0; h++) {
        for (uint8_t p = 0; p < snap[h].num_ports; p++) {
            if (strcmp(snap[h].ports[p].path, path) == 0) {
                state = snap[h].ports[p].powered ? 1 : 0;
                break;
            }
        }
    }
    free(snap);
    if (state < 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_DATA_OUT_OF_RANGE);
        return SCPI_RES_ERR;
    }
    SCPI_ResultInt32(ctx, state);
    return SCPI_RES_OK;
}

static scpi_result_t hub_port_cycle(scpi_t *ctx)
{
    char path[32];
    if (!param_text(ctx, path, sizeof(path), true)) {
        return SCPI_RES_ERR;
    }
    int32_t off_ms = 1000;
    SCPI_ParamInt32(ctx, &off_ms, false);
    char extra[16] = "";
    param_text(ctx, extra, sizeof(extra), false);
    if (off_ms < 0) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
        return SCPI_RES_ERR;
    }
    esp_err_t err = hub_ctl_port_cycle(path, (uint32_t)off_ms, strcasecmp(extra, "FORCE") == 0, NULL, 0);
    return err == ESP_OK ? SCPI_RES_OK : push_err(ctx, err);
}

static scpi_result_t usb_devices_q(scpi_t *ctx)
{
    size_t max = CONFIG_USBIP_MAX_DEVICES;
    usbip_backend_device_t *devs = malloc(max * sizeof(*devs));
    if (devs == NULL) {
        SCPI_ErrorPush(ctx, SCPI_ERROR_OUT_OF_MEMORY);
        return SCPI_RES_ERR;
    }
    size_t n = usb_backend_get_devices(devs, max);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char id[10];
        cJSON_AddStringToObject(o, "busid", devs[i].busid);
        snprintf(id, sizeof(id), "%04x", devs[i].id_vendor);
        cJSON_AddStringToObject(o, "vid", id);
        snprintf(id, sizeof(id), "%04x", devs[i].id_product);
        cJSON_AddStringToObject(o, "pid", id);
        cJSON_AddStringToObject(o, "product", devs[i].product);
        cJSON_AddStringToObject(o, "serial", devs[i].serial);
        cJSON_AddItemToArray(arr, o);
    }
    free(devs);
    return result_json(ctx, arr);
}

static scpi_result_t mux_select(scpi_t *ctx)
{
    char dut[SCPI_TEXT_LEN];
    if (!param_text(ctx, dut, sizeof(dut), true)) {
        return SCPI_RES_ERR;
    }
    esp_err_t err = analog_mux_select_dut(dut, NULL, 0);
    return err == ESP_OK ? SCPI_RES_OK : push_err(ctx, err);
}

static scpi_result_t mux_select_channel(scpi_t *ctx)
{
    char group[SCPI_TEXT_LEN];
    int32_t channel;
    if (!param_text(ctx, group, sizeof(group), true) || !SCPI_ParamInt32(ctx, &channel, true)) {
        return SCPI_RES_ERR;
    }
    esp_err_t err = analog_mux_select_channel(group, channel, NULL, 0, NULL, 0);
    return err == ESP_OK ? SCPI_RES_OK : push_err(ctx, err);
}

static scpi_result_t mux_isolate(scpi_t *ctx)
{
    esp_err_t err = analog_mux_isolate(NULL, 0);
    return err == ESP_OK ? SCPI_RES_OK : push_err(ctx, err);
}

static scpi_result_t mux_active_q(scpi_t *ctx)
{
    char active[SCPI_TEXT_LEN];
    analog_mux_get_active(active, sizeof(active));
    SCPI_ResultText(ctx, active);
    return SCPI_RES_OK;
}

static scpi_result_t mux_status_q(scpi_t *ctx)
{
    return result_json(ctx, analog_mux_describe());
}

static const scpi_command_t k_commands[] = {
    { .pattern = "HUB:LIST?",                 .callback = hub_list_q,         },
    { .pattern = "HUB:PORT:POWer",            .callback = hub_port_power,     },
    { .pattern = "HUB:PORT:POWer?",           .callback = hub_port_power_q,   },
    { .pattern = "HUB:PORT:CYCLe",            .callback = hub_port_cycle,     },
    { .pattern = "USB:DEVices?",              .callback = usb_devices_q,      },
    { .pattern = "MUX:SELect",                .callback = mux_select,         },
    { .pattern = "MUX:SELect:CHANnel",        .callback = mux_select_channel, },
    { .pattern = "MUX:ISOLate",               .callback = mux_isolate,        },
    { .pattern = "MUX:ACTive?",               .callback = mux_active_q,       },
    { .pattern = "MUX:STATus?",               .callback = mux_status_q,       },
    SCPI_CMD_LIST_END
};

const scpi_command_t *scpi_controller_commands(void)
{
    return k_commands;
}
