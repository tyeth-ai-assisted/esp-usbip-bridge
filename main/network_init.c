#include "network_init.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_USBIP_TRANSPORT_ETHERNET
#include "esp_eth.h"
#endif

#if CONFIG_USBIP_TRANSPORT_WIFI
#include "esp_wifi.h"
#endif

static const char *TAG = "network";

#if CONFIG_USBIP_TRANSPORT_ETHERNET
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
    } else if (event_id == ETHERNET_EVENT_START) {
        ESP_LOGI(TAG, "Ethernet started");
    } else if (event_id == ETHERNET_EVENT_STOP) {
        ESP_LOGW(TAG, "Ethernet stopped");
    }
}

static void got_eth_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet IPv4: " IPSTR, IP2STR(&event->ip_info.ip));
}

#if CONFIG_USBIP_ETH_PHY_YT8531_INIT
/* Read-modify-write a YT8531 extended register via EXT addr (0x1E) / data (0x1F). */
static esp_err_t yt8531_ext_reg_update(esp_eth_handle_t eth_handle, uint32_t ext_reg,
                                       uint32_t clear_mask, uint32_t set_bits)
{
    uint32_t reg_val = ext_reg;
    esp_eth_phy_reg_rw_data_t phy_reg = {.reg_addr = 0x1E, .reg_value_p = &reg_val};
    esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg);
    if (err != ESP_OK) {
        return err;
    }

    phy_reg.reg_addr = 0x1F;
    err = esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG, &phy_reg);
    if (err != ESP_OK) {
        return err;
    }

    reg_val = (reg_val & ~clear_mask) | set_bits;
    return esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg);
}

static esp_err_t yt8531_rgmii_init(esp_eth_handle_t eth_handle)
{
    /* The YT8531 comes out of the generic driver's reset with auto-negotiation off. */
    bool auto_nego_en = true;
    esp_err_t err = esp_eth_ioctl(eth_handle, ETH_CMD_S_AUTONEGO, &auto_nego_en);
    if (err != ESP_OK) {
        return err;
    }

    /* Rx: ~2 ns coarse delay, EXT_CHIP_CONFIG (0xA001) bit 8 rxc_dly_en. */
    err = yt8531_ext_reg_update(eth_handle, 0xA001, 0, 1U << 8);
    if (err != ESP_OK) {
        return err;
    }

    /* Tx: EXT_RGMII_CONFIG1 (0xA003) tx_delay_sel[3:0] and tx_delay_sel_fe[7:4]
     * = 13 steps x 150 ps (~1.95 ns). */
    err = yt8531_ext_reg_update(eth_handle, 0xA003, 0x00FF, (13U << 4) | 13U);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "YT8531 RGMII delays configured (Rx ~2 ns, Tx ~1.95 ns)");
    return ESP_OK;
}
#endif

static esp_err_t network_init_ethernet(void)
{
    esp_err_t err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_eth_ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_USBIP_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_USBIP_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_USBIP_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_USBIP_ETH_MDIO_GPIO;

#if CONFIG_USBIP_ETH_INTERFACE_RGMII
    emac_config.interface = EMAC_DATA_INTERFACE_RGMII;
    emac_config.clock_config.rgmii.clock_tx_gpio = CONFIG_USBIP_ETH_RGMII_TX_CLK_GPIO;
    emac_config.clock_config.rgmii.clock_rx_gpio = CONFIG_USBIP_ETH_RGMII_RX_CLK_GPIO;
    emac_config.clock_config.rgmii.clock_phy_ref_gpio = CONFIG_USBIP_ETH_RGMII_PHY_REF_CLK_GPIO;

    emac_config.emac_dataif_gpio.rgmii.tx_ctl_num = CONFIG_USBIP_ETH_RGMII_TX_CTL_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd0_num = CONFIG_USBIP_ETH_RGMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd1_num = CONFIG_USBIP_ETH_RGMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd2_num = CONFIG_USBIP_ETH_RGMII_TXD2_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd3_num = CONFIG_USBIP_ETH_RGMII_TXD3_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rx_ctl_num = CONFIG_USBIP_ETH_RGMII_RX_CTL_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd0_num = CONFIG_USBIP_ETH_RGMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd1_num = CONFIG_USBIP_ETH_RGMII_RXD1_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd2_num = CONFIG_USBIP_ETH_RGMII_RXD2_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd3_num = CONFIG_USBIP_ETH_RGMII_RXD3_GPIO;

    ESP_LOGI(TAG,
             "EMAC config: interface=RGMII TXC=%d RXC=%d REF_CLK=%d "
             "MDC=%d MDIO=%d PHY_ADDR=%d RST=%d "
             "TX_CTL=%d TXD0-3=%d,%d,%d,%d RX_CTL=%d RXD0-3=%d,%d,%d,%d",
             emac_config.clock_config.rgmii.clock_tx_gpio,
             emac_config.clock_config.rgmii.clock_rx_gpio,
             emac_config.clock_config.rgmii.clock_phy_ref_gpio,
             emac_config.smi_gpio.mdc_num,
             emac_config.smi_gpio.mdio_num,
             phy_config.phy_addr,
             phy_config.reset_gpio_num,
             emac_config.emac_dataif_gpio.rgmii.tx_ctl_num,
             emac_config.emac_dataif_gpio.rgmii.txd0_num,
             emac_config.emac_dataif_gpio.rgmii.txd1_num,
             emac_config.emac_dataif_gpio.rgmii.txd2_num,
             emac_config.emac_dataif_gpio.rgmii.txd3_num,
             emac_config.emac_dataif_gpio.rgmii.rx_ctl_num,
             emac_config.emac_dataif_gpio.rgmii.rxd0_num,
             emac_config.emac_dataif_gpio.rgmii.rxd1_num,
             emac_config.emac_dataif_gpio.rgmii.rxd2_num,
             emac_config.emac_dataif_gpio.rgmii.rxd3_num);
#else
    // Override RMII clock configuration
#if CONFIG_USBIP_ETH_RMII_CLK_MODE == 0
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
#else
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
#endif
    emac_config.clock_config.rmii.clock_gpio = CONFIG_USBIP_ETH_RMII_CLK_GPIO;

    // Override RMII data plane GPIOs (ESP32-P4 uses multi-IOMUX)
    emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_USBIP_ETH_RMII_TX_EN_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_USBIP_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_USBIP_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_USBIP_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_USBIP_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_USBIP_ETH_RMII_RXD1_GPIO;

    ESP_LOGI(TAG,
             "EMAC config: interface=%d clock_mode=%d clock_gpio=%d "
             "MDC=%d MDIO=%d PHY_ADDR=%d RST=%d "
             "TX_EN=%d TXD0=%d TXD1=%d CRS_DV=%d RXD0=%d RXD1=%d",
             emac_config.interface,
             emac_config.clock_config.rmii.clock_mode,
             emac_config.clock_config.rmii.clock_gpio,
             emac_config.smi_gpio.mdc_num,
             emac_config.smi_gpio.mdio_num,
             phy_config.phy_addr,
             phy_config.reset_gpio_num,
             emac_config.emac_dataif_gpio.rmii.tx_en_num,
             emac_config.emac_dataif_gpio.rmii.txd0_num,
             emac_config.emac_dataif_gpio.rmii.txd1_num,
             emac_config.emac_dataif_gpio.rmii.crs_dv_num,
             emac_config.emac_dataif_gpio.rmii.rxd0_num,
             emac_config.emac_dataif_gpio.rmii.rxd1_num);
#endif

    ESP_LOGI(TAG, "Creating EMAC MAC instance...");

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Creating PHY instance...");

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Installing Ethernet driver...");
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_USBIP_ETH_PHY_YT8531_INIT
    err = yt8531_rgmii_init(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "YT8531 PHY init failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    ESP_LOGI(TAG, "Attaching netif...");
    err = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Starting Ethernet...");
    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "Ethernet initialized (MDC=%d MDIO=%d PHY_ADDR=%d RST=%d)",
             CONFIG_USBIP_ETH_MDC_GPIO,
             CONFIG_USBIP_ETH_MDIO_GPIO,
             CONFIG_USBIP_ETH_PHY_ADDR,
             CONFIG_USBIP_ETH_PHY_RST_GPIO);
    return ESP_OK;
}
#endif

#if CONFIG_USBIP_TRANSPORT_WIFI

#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"
#define WIFI_CRED_MAX_LEN  64
#define WIFI_PROMPT_TIMEOUT_MS 3000

/* Read a line from UART stdin, stripping trailing newline. Returns length. */
static int uart_read_line(const char *prompt, char *buf, size_t buf_size, bool echo)
{
    printf("%s", prompt);
    fflush(stdout);

    int i = 0;
    while (i < (int)(buf_size - 1)) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (echo) {
                printf("\n");
            }
            break;
        }
        if (c == '\b' || c == 127) {  /* backspace / DEL */
            if (i > 0) {
                i--;
                if (echo) {
                    printf("\b \b");
                    fflush(stdout);
                }
            }
            continue;
        }
        buf[i++] = (char)c;
        if (echo) {
            printf("%c", c);
        } else {
            printf("*");
        }
        fflush(stdout);
    }
    buf[i] = '\0';
    return i;
}

/* Check if the user presses a key within timeout_ms. */
static bool uart_wait_for_keypress(int timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int c = fgetc(stdin);
        if (c != EOF) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static esp_err_t wifi_creds_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t wifi_creds_load(char *ssid, size_t ssid_size, char *password, size_t pass_size)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs, WIFI_NVS_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_NVS_KEY_PASS, password, &pass_size);
    }
    nvs_close(nvs);
    return err;
}

static void wifi_prompt_credentials(char *ssid, size_t ssid_size, char *password, size_t pass_size)
{
    printf("\n--- Wi-Fi Configuration ---\n");
    while (true) {
        uart_read_line("SSID: ", ssid, ssid_size, true);
        if (strlen(ssid) > 0) break;
        printf("SSID cannot be empty.\n");
    }
    uart_read_line("Password (empty for open): ", password, pass_size, false);
    printf("\n");
}

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static int s_wifi_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < CONFIG_USBIP_WIFI_MAX_RETRY) {
            (void)esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", s_wifi_retry_count, CONFIG_USBIP_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi IPv4: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static wifi_auth_mode_t wifi_authmode_from_config(void)
{
    switch (CONFIG_USBIP_WIFI_AUTHMODE) {
    case 0:
        return WIFI_AUTH_OPEN;
    case 1:
        return WIFI_AUTH_WPA_PSK;
    case 2:
        return WIFI_AUTH_WPA2_PSK;
    case 3:
    default:
        return WIFI_AUTH_WPA2_WPA3_PSK;
    }
}

static esp_err_t wifi_attempt_connect(const char *ssid, const char *password)
{
    s_wifi_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wifi_authmode_from_config(),
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_USBIP_WIFI_DISABLE_MODEM_SLEEP
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected to SSID \"%s\"", ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi failed to connect to SSID \"%s\"", ssid);
    esp_wifi_stop();
    return ESP_FAIL;
}

static esp_err_t network_init_wifi(void)
{
    char ssid[WIFI_CRED_MAX_LEN] = {0};
    char password[WIFI_CRED_MAX_LEN] = {0};
    bool need_prompt = false;

    /* Try loading credentials from NVS */
    esp_err_t err = wifi_creds_load(ssid, sizeof(ssid), password, sizeof(password));
    if (err == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Saved Wi-Fi SSID: \"%s\"", ssid);
        printf("Press any key within 3s to reconfigure Wi-Fi...\n");
        if (uart_wait_for_keypress(WIFI_PROMPT_TIMEOUT_MS)) {
            need_prompt = true;
        }
    } else {
        /* No saved credentials — fall back to Kconfig if set */
        if (strlen(CONFIG_USBIP_WIFI_SSID) > 0) {
            strlcpy(ssid, CONFIG_USBIP_WIFI_SSID, sizeof(ssid));
            strlcpy(password, CONFIG_USBIP_WIFI_PASSWORD, sizeof(password));
            ESP_LOGI(TAG, "Using Kconfig Wi-Fi SSID: \"%s\"", ssid);
            printf("Press any key within 3s to reconfigure Wi-Fi...\n");
            if (uart_wait_for_keypress(WIFI_PROMPT_TIMEOUT_MS)) {
                need_prompt = true;
            }
        } else {
            need_prompt = true;
        }
    }

    if (need_prompt) {
        wifi_prompt_credentials(ssid, sizeof(ssid), password, sizeof(password));
        err = wifi_creds_save(ssid, password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save Wi-Fi credentials to NVS: %s", esp_err_to_name(err));
        }
    }

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty.");
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Connection loop: retry with new credentials on failure */
    while (true) {
        err = wifi_attempt_connect(ssid, password);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        printf("\nWi-Fi connection failed. Enter new credentials.\n");
        wifi_prompt_credentials(ssid, sizeof(ssid), password, sizeof(password));
        err = wifi_creds_save(ssid, password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save Wi-Fi credentials to NVS: %s", esp_err_to_name(err));
        }
    }
}
#endif

esp_err_t network_init_start(void)
{
#if CONFIG_USBIP_TRANSPORT_ETHERNET
    return network_init_ethernet();
#elif CONFIG_USBIP_TRANSPORT_WIFI
    return network_init_wifi();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
