#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_console.h"

static const char *TAG = "WIFI_MGR";

static esp_netif_t *s_sta_netif = NULL;
static bool s_connected = false;
static char s_ip_str[20] = {0};
static char s_current_ssid[33] = APP_WIFI_DEFAULT_SSID;
static wifi_state_callback_t s_state_cb = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started. Connecting to SSID: '%s'...", s_current_ssid);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason: %d). Reconnecting in 2 seconds...", disc->reason);
            s_connected = false;
            memset(s_ip_str, 0, sizeof(s_ip_str));

            if (s_state_cb) {
                s_state_cb(false, "");
            }

            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            esp_ip4addr_ntoa(&event->ip_info.ip, s_ip_str, sizeof(s_ip_str));
            s_connected = true;

            ESP_LOGI(TAG, "=================================================");
            ESP_LOGI(TAG, "Wi-Fi Connected successfully!");
            ESP_LOGI(TAG, "SSID:        %s", s_current_ssid);
            ESP_LOGI(TAG, "IP Address:  %s", s_ip_str);
            ESP_LOGI(TAG, "Netmask:     " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "Gateway:     " IPSTR, IP2STR(&event->ip_info.gw));
            ESP_LOGI(TAG, "Web UI at:   http://%s", s_ip_str);
            ESP_LOGI(TAG, "=================================================");

            if (s_state_cb) {
                s_state_cb(true, s_ip_str);
            }
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Wi-Fi subsystem...");

    /* Initialize TCP/IP stack */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize default event loop */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create default station netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA netif");
        return ESP_FAIL;
    }

    /* Initialize Wi-Fi driver */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    /* Configure Station */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = APP_WIFI_DEFAULT_SSID,
            .password = APP_WIFI_DEFAULT_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    strncpy(s_current_ssid, APP_WIFI_DEFAULT_SSID, sizeof(s_current_ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    /* Start Wi-Fi */
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Wi-Fi subsystem initialized for STA mode.");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t max_len)
{
    if (!s_connected || s_ip_str[0] == '\0') {
        return ESP_FAIL;
    }
    strncpy(ip_str, s_ip_str, max_len - 1);
    ip_str[max_len - 1] = '\0';
    return ESP_OK;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!s_connected) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

const char *wifi_manager_get_ssid(void)
{
    return s_current_ssid;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Changing Wi-Fi network to '%s'...", ssid);
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = password && strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid) - 1);
    s_connected = false;
    memset(s_ip_str, 0, sizeof(s_ip_str));

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    return esp_wifi_connect();
}

void wifi_manager_register_state_callback(wifi_state_callback_t cb)
{
    s_state_cb = cb;
}

/* --- CONSOLE COMMANDS --- */

static int cmd_wifi(int argc, char **argv)
{
    printf("\n=== Wi-Fi Status ===\n");
    printf("State:       %s\n", s_connected ? "CONNECTED" : "DISCONNECTED / CONNECTING");
    printf("SSID:        %s\n", s_current_ssid);
    if (s_connected) {
        printf("IP:          %s\n", s_ip_str);
        printf("RSSI:        %d dBm\n", (int)wifi_manager_get_rssi());
        printf("Web UI:      http://%s\n", s_ip_str);
    }
    
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        printf("STA MAC:     %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    printf("====================\n\n");
    return 0;
}

static int cmd_wifi_connect(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: wifi_connect <ssid> [password]\n");
        return 1;
    }
    const char *ssid = argv[1];
    const char *pass = (argc >= 3) ? argv[2] : "";
    printf("Connecting to '%s'...\n", ssid);
    esp_err_t err = wifi_manager_connect(ssid, pass);
    if (err != ESP_OK) {
        printf("Failed to start connection: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Connection initiated. Run 'wifi' to monitor status.\n");
    return 0;
}

void wifi_manager_register_console_commands(void)
{
    esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Show Wi-Fi connection status, IP address, and signal strength",
        .hint = NULL,
        .func = &cmd_wifi,
    };
    esp_console_cmd_register(&wifi_cmd);

    esp_console_cmd_t connect_cmd = {
        .command = "wifi_connect",
        .help = "Connect to a Wi-Fi Access Point",
        .hint = "<ssid> [password]",
        .func = &cmd_wifi_connect,
    };
    esp_console_cmd_register(&connect_cmd);
}
