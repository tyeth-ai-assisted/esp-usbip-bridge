#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

#include "analog_mux.h"
#include "controller_api.h"
#include "hub_control.h"
#include "device_naming.h"
#include "discovery_service.h"
#include "http_server.h"
// #include "log_server.h"
#include "neopixel.h"
#include "network_init.h"
#include "usb_backend.h"
#include "usbip_server.h"

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
#include "harness_io_expander.h"
#endif
#if CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER
#include "virtual_perfetto_logic.h"
#endif
#if CONFIG_USBIP_VIRTUAL_HARNESS
#include "virtual_harness.h"
#endif

static const char *TAG = "app";

void app_main(void)
{
    /* Blink neopixel to confirm this firmware is running. */
    neopixel_init(CONFIG_USBIP_NEOPIXEL_GPIO);
    for (int i = 0; i < 3; i++) {
        neopixel_set_rgb(0, 64, 0);
        esp_rom_delay_us(200000);
        neopixel_off();
        esp_rom_delay_us(200000);
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(device_naming_init());
    ESP_ERROR_CHECK(controller_api_init());
    ESP_ERROR_CHECK(analog_mux_init());
    /* Before usb_backend_start(): installs the port power restore policy */
    ESP_ERROR_CHECK(hub_ctl_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(network_init_start());
    // ESP_ERROR_CHECK(log_server_start());
    /* IO expander must be initialised before usb_backend_start() so that
       usb_backend_p4hil_usb_power_init() can enable USB host VBUS power.
       Initialise before http_server_start() too, so the HTTP task never
       races with us to create the I2C bus. */
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_USBIP_P4HIL_USB_POWER_ENABLE
    {
        const uint8_t addrs[3] = {0x20, 0x21, 0x22};
        esp_err_t err = harness_io_expander_init(35, 36, addrs, 3, 100000);
        ESP_ERROR_CHECK(err);
    }
#endif

    ESP_ERROR_CHECK(http_server_start());

    ESP_ERROR_CHECK(usb_backend_start());
#if CONFIG_USBIP_VIRTUAL_LOGIC_ANALYZER
    ESP_ERROR_CHECK(virtual_perfetto_logic_start());
#endif
#if CONFIG_USBIP_VIRTUAL_HARNESS
    ESP_ERROR_CHECK(virtual_harness_start());
#endif
    ESP_ERROR_CHECK(discovery_service_start());
    ESP_ERROR_CHECK(usbip_server_start());

    ESP_LOGI(TAG, "USB/IP bridge started");
}
