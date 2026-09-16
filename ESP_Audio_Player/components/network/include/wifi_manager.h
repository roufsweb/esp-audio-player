#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_WIFI_DEFAULT_SSID
#define APP_WIFI_DEFAULT_SSID "rouf.iot"
#endif

#ifndef APP_WIFI_DEFAULT_PASSWORD
#define APP_WIFI_DEFAULT_PASSWORD "55555555"
#endif

typedef void (*wifi_state_callback_t)(bool connected, const char *ip_str);

/**
 * @brief Initialize Wi-Fi Station mode and connect to default credentials.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Check if Wi-Fi STA is currently connected and has acquired an IP.
 *
 * @return true if connected with valid IP, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get the current IPv4 address string (e.g. "192.168.1.50").
 *
 * @param ip_str Destination buffer
 * @param max_len Size of buffer (at least 16 bytes recommended)
 * @return ESP_OK on success, ESP_FAIL if not connected
 */
esp_err_t wifi_manager_get_ip(char *ip_str, size_t max_len);

/**
 * @brief Get the current Wi-Fi RSSI in dBm.
 *
 * @return RSSI value (e.g. -55 dBm) or 0 if not connected
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief Get the configured / connected SSID string.
 *
 * @return Null-terminated string
 */
const char *wifi_manager_get_ssid(void);

/**
 * @brief Connect or reconnect to specified SSID and password.
 *
 * @param ssid Wi-Fi network name
 * @param password Wi-Fi network password
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Register a callback for Wi-Fi connection/disconnection events.
 *
 * @param cb Callback function
 */
void wifi_manager_register_state_callback(wifi_state_callback_t cb);

/**
 * @brief Register Wi-Fi diagnostic console commands:
 *        - wifi : Show connection status, SSID, IP, RSSI
 *        - wifi_connect <ssid> <pass> : Connect to specified AP
 */
void wifi_manager_register_console_commands(void);

#ifdef __cplusplus
}
#endif
