#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_MAX_DISCOVERED_DEVICES 20

typedef enum {
    BT_MGR_A2D_STATE_DISCONNECTED = 0,
    BT_MGR_A2D_STATE_CONNECTING,
    BT_MGR_A2D_STATE_CONNECTED,
    BT_MGR_A2D_STATE_DISCONNECTING
} bt_mgr_a2d_state_t;

typedef enum {
    BT_MGR_AUDIO_STATE_SUSPEND = 0,
    BT_MGR_AUDIO_STATE_STARTED
} bt_mgr_audio_state_t;

typedef struct {
    esp_bd_addr_t bda;
    char bda_str[18];
    char name[64];
    int8_t rssi;
} bt_device_entry_t;

typedef struct {
    bt_mgr_a2d_state_t a2d_state;
    bt_mgr_audio_state_t audio_state;
    bool is_scanning;
    char connected_name[64];
    char connected_bda_str[18];
    uint8_t discovered_count;
} bt_mgr_status_t;

/**
 * @brief Initialize Bluetooth manager state variables and locks.
 */
esp_err_t bt_manager_init(void);

/**
 * @brief GAP event callback passed to esp_bt_gap_register_callback.
 */
void bt_manager_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

/**
 * @brief A2DP event callback passed to esp_a2d_register_callback.
 */
void bt_manager_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/**
 * @brief Start Bluetooth GAP discovery/scan for nearby devices.
 *
 * @param duration_sec Scan duration in inquiry units (approx. duration_sec * 1.28s)
 * @return ESP_OK on success
 */
esp_err_t bt_manager_start_scan(uint8_t duration_sec);

/**
 * @brief Stop active GAP discovery.
 */
esp_err_t bt_manager_stop_scan(void);

/**
 * @brief Check if Bluetooth discovery is currently in progress.
 */
bool bt_manager_is_scanning(void);

/**
 * @brief Connect to an A2DP Sink device by raw MAC address.
 */
esp_err_t bt_manager_connect(const esp_bd_addr_t bda);

/**
 * @brief Connect to an A2DP Sink device by formatted string (e.g. "AA:BB:CC:DD:EE:FF").
 */
esp_err_t bt_manager_connect_str(const char *mac_str);

/**
 * @brief Disconnect current A2DP connection.
 */
esp_err_t bt_manager_disconnect(void);

/**
 * @brief Get current Bluetooth system and connection status.
 */
void bt_manager_get_status(bt_mgr_status_t *out_status);

/**
 * @brief Get copy of all discovered devices.
 *
 * @param out_devices Array to populate
 * @param max_count Maximum items to copy
 * @return Number of devices copied
 */
size_t bt_manager_get_discovered_devices(bt_device_entry_t *out_devices, size_t max_count);

/**
 * @brief Clear discovered devices list.
 */
void bt_manager_clear_discovered_devices(void);

#ifdef __cplusplus
}
#endif
