#include "bt_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "BT_MGR";

static SemaphoreHandle_t s_bt_lock = NULL;
static bool s_is_scanning = false;
static bt_mgr_a2d_state_t s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
static bt_mgr_audio_state_t s_audio_state = BT_MGR_AUDIO_STATE_SUSPEND;

static esp_bd_addr_t s_connected_bda = {0};
static char s_connected_bda_str[18] = {0};
static char s_connected_name[64] = {0};

static bt_device_entry_t s_devices[BT_MAX_DISCOVERED_DEVICES];
static uint8_t s_device_count = 0;

esp_err_t bt_manager_init(void)
{
    if (s_bt_lock == NULL) {
        s_bt_lock = xSemaphoreCreateMutex();
        if (s_bt_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_is_scanning = false;
    s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
    s_audio_state = BT_MGR_AUDIO_STATE_SUSPEND;
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    ESP_LOGI(TAG, "Bluetooth Manager initialized successfully.");
    return ESP_OK;
}

void bt_manager_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[64] = {0};
        int8_t rssi = -128;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int len = param->disc_res.prop[i].len;
                if (len > 63) len = 63;
                memcpy(name, param->disc_res.prop[i].val, len);
                name[len] = '\0';
            } else if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_RSSI) {
                rssi = *(int8_t *)(param->disc_res.prop[i].val);
            }
        }

        char bda_str[18];
        snprintf(bda_str, sizeof(bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                 param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);

        if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            int existing_idx = -1;
            for (int i = 0; i < s_device_count; i++) {
                if (memcmp(s_devices[i].bda, param->disc_res.bda, ESP_BD_ADDR_LEN) == 0) {
                    existing_idx = i;
                    break;
                }
            }

            if (existing_idx >= 0) {
                if (name[0] != '\0') {
                    strncpy(s_devices[existing_idx].name, name, sizeof(s_devices[existing_idx].name) - 1);
                }
                if (rssi != -128) {
                    s_devices[existing_idx].rssi = rssi;
                }
            } else if (s_device_count < BT_MAX_DISCOVERED_DEVICES) {
                memcpy(s_devices[s_device_count].bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                strncpy(s_devices[s_device_count].bda_str, bda_str, sizeof(s_devices[s_device_count].bda_str) - 1);
                if (name[0] != '\0') {
                    strncpy(s_devices[s_device_count].name, name, sizeof(s_devices[s_device_count].name) - 1);
                } else {
                    strncpy(s_devices[s_device_count].name, "Unnamed Device", sizeof(s_devices[s_device_count].name) - 1);
                }
                s_devices[s_device_count].rssi = rssi;
                s_device_count++;
                ESP_LOGI(TAG, "Discovered Device #%d: '%s' [%s] RSSI: %d",
                         s_device_count, name[0] ? name : "Unnamed", bda_str, rssi);
            }
            xSemaphoreGive(s_bt_lock);
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_is_scanning = false;
            ESP_LOGI(TAG, "Bluetooth discovery completed. Total devices found: %d", s_device_count);
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_is_scanning = true;
            ESP_LOGI(TAG, "Bluetooth discovery started...");
        }
        break;

    default:
        break;
    }
}

void bt_manager_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                s_a2d_state = BT_MGR_A2D_STATE_CONNECTED;
                memcpy(s_connected_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
                snprintf(s_connected_bda_str, sizeof(s_connected_bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1], param->conn_stat.remote_bda[2],
                         param->conn_stat.remote_bda[3], param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);

                /* Search discovered devices for name */
                s_connected_name[0] = '\0';
                for (int i = 0; i < s_device_count; i++) {
                    if (memcmp(s_devices[i].bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN) == 0) {
                        strncpy(s_connected_name, s_devices[i].name, sizeof(s_connected_name) - 1);
                        break;
                    }
                }
                if (s_connected_name[0] == '\0') {
                    strncpy(s_connected_name, "Bluetooth Audio Sink", sizeof(s_connected_name) - 1);
                }
                ESP_LOGI(TAG, "A2DP Connected: %s [%s]", s_connected_name, s_connected_bda_str);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                s_a2d_state = BT_MGR_A2D_STATE_CONNECTING;
                ESP_LOGI(TAG, "A2DP Connecting...");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTING;
                ESP_LOGI(TAG, "A2DP Disconnecting...");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
                memset(s_connected_bda, 0, sizeof(s_connected_bda));
                s_connected_bda_str[0] = '\0';
                s_connected_name[0] = '\0';
                ESP_LOGI(TAG, "A2DP Disconnected.");
            }
            xSemaphoreGive(s_bt_lock);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_audio_state = BT_MGR_AUDIO_STATE_STARTED;
            ESP_LOGI(TAG, "A2DP Audio Stream: STARTED");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            s_audio_state = BT_MGR_AUDIO_STATE_SUSPEND;
            ESP_LOGI(TAG, "A2DP Audio Stream: SUSPENDED");
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
            s_audio_state = BT_MGR_AUDIO_STATE_SUSPEND;
            ESP_LOGI(TAG, "A2DP Audio Stream: STOPPED");
        }
        break;

    default:
        break;
    }
}

esp_err_t bt_manager_start_scan(uint8_t duration_sec)
{
    if (duration_sec < 1) duration_sec = 10;
    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_device_count = 0;
        memset(s_devices, 0, sizeof(s_devices));
        xSemaphoreGive(s_bt_lock);
    }
    ESP_LOGI(TAG, "Starting GAP discovery scan (duration: %d units)...", duration_sec);
    return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, duration_sec, 0);
}

esp_err_t bt_manager_stop_scan(void)
{
    return esp_bt_gap_cancel_discovery();
}

bool bt_manager_is_scanning(void)
{
    return s_is_scanning;
}

esp_err_t bt_manager_connect(const esp_bd_addr_t bda)
{
    s_a2d_state = BT_MGR_A2D_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x...",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return esp_a2d_source_connect((uint8_t *)bda);
}

esp_err_t bt_manager_connect_str(const char *mac_str)
{
    if (!mac_str || strlen(mac_str) < 17) {
        return ESP_ERR_INVALID_ARG;
    }
    int mac[6];
    if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        esp_bd_addr_t bda;
        for (int i = 0; i < 6; i++) {
            bda[i] = (uint8_t)mac[i];
        }
        return bt_manager_connect(bda);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t bt_manager_disconnect(void)
{
    if (s_a2d_state == BT_MGR_A2D_STATE_DISCONNECTED) {
        return ESP_OK;
    }
    s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTING;
    ESP_LOGI(TAG, "Disconnecting from current A2DP sink...");
    return esp_a2d_source_disconnect(s_connected_bda);
}

void bt_manager_get_status(bt_mgr_status_t *out_status)
{
    if (!out_status) return;
    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        out_status->a2d_state = s_a2d_state;
        out_status->audio_state = s_audio_state;
        out_status->is_scanning = s_is_scanning;
        strncpy(out_status->connected_name, s_connected_name, sizeof(out_status->connected_name) - 1);
        strncpy(out_status->connected_bda_str, s_connected_bda_str, sizeof(out_status->connected_bda_str) - 1);
        out_status->discovered_count = s_device_count;
        xSemaphoreGive(s_bt_lock);
    } else {
        out_status->a2d_state = s_a2d_state;
        out_status->audio_state = s_audio_state;
        out_status->is_scanning = s_is_scanning;
        out_status->connected_name[0] = '\0';
        out_status->connected_bda_str[0] = '\0';
        out_status->discovered_count = 0;
    }
}

size_t bt_manager_get_discovered_devices(bt_device_entry_t *out_devices, size_t max_count)
{
    if (!out_devices || max_count == 0) return 0;
    size_t copied = 0;
    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        copied = s_device_count < max_count ? s_device_count : max_count;
        memcpy(out_devices, s_devices, copied * sizeof(bt_device_entry_t));
        xSemaphoreGive(s_bt_lock);
    }
    return copied;
}

void bt_manager_clear_discovered_devices(void)
{
    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_device_count = 0;
        memset(s_devices, 0, sizeof(s_devices));
        xSemaphoreGive(s_bt_lock);
    }
}
