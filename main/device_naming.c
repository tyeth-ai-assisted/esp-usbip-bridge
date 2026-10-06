#include "device_naming.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "device_naming";

/* ─────────────────────────────────────────────
 *  NVS helpers
 * ───────────────────────────────────────────── */

static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    return nvs_open(DEVICE_NVS_NAMESPACE, NVS_READWRITE, h);
}

static esp_err_t nvs_open_ro(nvs_handle_t *h)
{
    return nvs_open(DEVICE_NVS_NAMESPACE, NVS_READONLY, h);
}

/* Build the NVS key for a per-device friendly name.
   busid can contain characters like '-' which NVS keys cannot,
   so we hex-encode it: "name_" + hex(busid).
   The result is placed in key_buf (max 64 bytes). */
static void device_name_key(const char *busid, char *key_buf, size_t key_size)
{
    /* "name_" prefix + 2 hex chars per byte + NUL */
    size_t busid_len = strlen(busid);
    size_t needed = 5 + busid_len * 2 + 1;
    if (key_size < needed) {
        key_buf[0] = '\0';
        return;
    }
    memcpy(key_buf, "name_", 5);
    for (size_t i = 0; i < busid_len; i++) {
        snprintf(key_buf + 5 + i * 2, 3, "%02x", (unsigned char)busid[i]);
    }
    key_buf[5 + busid_len * 2] = '\0';
}

/* Build the default MAC-based hostname.  ESP_MAC_BASE is always 6 bytes;
 * esp_efuse_mac_get_default() returns an 8-byte EUI-64 on 802.15.4 chips
 * such as the ESP32-S31. */
static void default_hostname(char *buf, size_t buf_size)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
    if (err != ESP_OK) {
        /* Fallback: "usbip-000000" */
        strlcpy(buf, "usbip-000000", buf_size);
        return;
    }
    snprintf(buf, buf_size, "usbip-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* ─────────────────────────────────────────────
 *  Public API
 * ───────────────────────────────────────────── */

esp_err_t device_naming_get_hostname(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        /* NVS not available — use default */
        default_hostname(buf, buf_size);
        return ESP_OK;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, DEVICE_NVS_KEY_HOSTNAME, buf, &len);
    nvs_close(h);

    if (err == ESP_OK && strlen(buf) > 0) {
        ESP_LOGI(TAG, "Using stored hostname: \"%s\"", buf);
        return ESP_OK;
    }

    /* No stored hostname — fall back to MAC-based default */
    default_hostname(buf, buf_size);
    return ESP_OK;
}

esp_err_t device_naming_set_hostname(const char *hostname)
{
    if (hostname == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(hostname) == 0) {
        /* Clear the stored hostname so get_hostname() falls back to default */
        err = nvs_erase_key(h, DEVICE_NVS_KEY_HOSTNAME);
    } else {
        err = nvs_set_str(h, DEVICE_NVS_KEY_HOSTNAME, hostname);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Hostname set to: \"%s\"", strlen(hostname) ? hostname : "(default)");
    }
    return err;
}

esp_err_t device_naming_get_board_id(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return err;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, DEVICE_NVS_KEY_BOARD_ID, buf, &len);
    nvs_close(h);

    if (err == ESP_OK) {
        /* Found a stored value */
        return ESP_OK;
    }

    /* Not found — return empty string */
    buf[0] = '\0';
    return ESP_OK;
}

esp_err_t device_naming_set_board_id(const char *board_id)
{
    if (board_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(board_id) == 0) {
        err = nvs_erase_key(h, DEVICE_NVS_KEY_BOARD_ID);
    } else {
        err = nvs_set_str(h, DEVICE_NVS_KEY_BOARD_ID, board_id);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Board ID set to: \"%s\"", strlen(board_id) ? board_id : "(cleared)");
    }
    return err;
}

esp_err_t device_naming_get_device_name(const char *busid, char *buf, size_t buf_size)
{
    if (busid == NULL || buf == NULL || buf_size < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[64];
    device_name_key(busid, key, sizeof(key));
    if (key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_ro(&h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len = buf_size;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);

    return err;
}

esp_err_t device_naming_set_device_name(const char *busid, const char *name)
{
    if (busid == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[64];
    device_name_key(busid, key, sizeof(key));
    if (key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) {
        return err;
    }

    if (strlen(name) == 0) {
        err = nvs_erase_key(h, key);
    } else {
        err = nvs_set_str(h, key, name);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device \"%s\" name set to: \"%s\"", busid, strlen(name) ? name : "(cleared)");
    }
    return err;
}

esp_err_t device_naming_init(void)
{
    /* Nothing needed at init time beyond what the per-call functions do.
       The NVS partition is already initialised by main.c before we are
       called. */
    return ESP_OK;
}
