#include "bt_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

static const char *TAG = "BT_MGR";

static SemaphoreHandle_t s_bt_lock = NULL;
static bool s_is_scanning = false;
static bt_mgr_a2d_state_t s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
static bt_mgr_audio_state_t s_audio_state = BT_MGR_AUDIO_STATE_SUSPEND;

static bool s_avrc_connected = false;
static uint8_t s_avrc_tl = 0;
static uint8_t s_current_volume_pct = 5;

static esp_bd_addr_t s_connected_bda = {0};

static char s_connected_bda_str[24] = {0};
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
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    memset(s_connected_bda_str, 0, sizeof(s_connected_bda_str));
    memset(s_connected_name, 0, sizeof(s_connected_name));
    s_avrc_connected = false;
    s_avrc_tl = 0;
    s_current_volume_pct = 5;


    /* Configure Secure Simple Pairing (SSP) with "No Input No Output" for auto Just Works pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    /* Set default PIN response */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    ESP_LOGI(TAG, "Bluetooth Manager initialized with SSP & auto-pairing support.");
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
            ESP_LOGI(TAG, "Bluetooth discovery stopped. Total devices found: %d", s_device_count);
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_is_scanning = true;
            ESP_LOGI(TAG, "Bluetooth discovery started...");
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT (Just Works confirmation for %06lu). Auto-confirming...",
                 (unsigned long)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06lu", (unsigned long)param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT: passkey requested");
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "ESP_BT_GAP_PIN_REQ_EVT: replying default PIN 0000");
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;
    }

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Bluetooth Authentication SUCCESS with '%s'", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Bluetooth Authentication FAILED with status: %d", param->auth_cmpl.stat);
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
                ESP_LOGI(TAG, "A2DP Connected successfully: %s [%s]", s_connected_name, s_connected_bda_str);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                s_a2d_state = BT_MGR_A2D_STATE_CONNECTING;
                ESP_LOGI(TAG, "A2DP Connecting in progress...");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
                s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTING;
                ESP_LOGI(TAG, "A2DP Disconnecting in progress...");
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
                memset(s_connected_bda, 0, sizeof(s_connected_bda));
                s_connected_bda_str[0] = '\0';
                s_connected_name[0] = '\0';
                ESP_LOGW(TAG, "A2DP Disconnected (reason: %d)", param->conn_stat.disc_rsn);
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

void bt_manager_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "AVRCP Controller connected to target device.");
            s_avrc_connected = true;
            /* Immediately synchronize hardware amplifier volume to current level */
            bt_manager_set_volume(s_current_volume_pct);
        } else {
            ESP_LOGI(TAG, "AVRCP Controller disconnected.");
            s_avrc_connected = false;
        }
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        ESP_LOGI(TAG, "AVRCP Target confirmed Absolute Volume: %d/127 (%.1f%%)",
                 param->set_volume_rsp.volume,
                 (param->set_volume_rsp.volume * 100.0) / 127.0);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            ESP_LOGI(TAG, "AVRCP Target reported hardware volume change: %d/127",
                     param->change_ntf.event_parameter.volume);
        }
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "AVRCP Remote features: 0x%08" PRIx32, param->rmt_feats.feat_mask);
        break;

    default:
        break;
    }
}

esp_err_t bt_manager_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;
    s_current_volume_pct = volume_pct;

    if (!s_avrc_connected) {
        /* AVRCP not yet connected, saved for sync upon connection */
        return ESP_OK;
    }

    uint8_t avrc_vol = (uint8_t)((volume_pct * 127 + 50) / 100);
    uint8_t tl = (s_avrc_tl++) & 0x0F;
    esp_err_t ret = esp_avrc_ct_send_set_absolute_volume_cmd(tl, avrc_vol);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send AVRCP set_absolute_volume (%d/127): %s",
                 avrc_vol, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Sent AVRCP SetAbsoluteVolume: %d/127 (TL: %u)", avrc_vol, tl);
    }
    return ret;
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
    /* 1. Stop active inquiry scan immediately if running so baseband radio can page target */
    if (s_is_scanning) {
        ESP_LOGI(TAG, "Stopping active inquiry scan before initiating A2DP connection...");
        esp_bt_gap_cancel_discovery();
        s_is_scanning = false;
        vTaskDelay(pdMS_TO_TICKS(150)); /* Allow radio baseband to settle */
    }

    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        /* 2. Disconnect previous connection if any */
        if (s_a2d_state == BT_MGR_A2D_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Disconnecting previous A2DP link...");
            esp_a2d_source_disconnect(s_connected_bda);
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        s_a2d_state = BT_MGR_A2D_STATE_CONNECTING;
        memcpy(s_connected_bda, bda, ESP_BD_ADDR_LEN);
        snprintf(s_connected_bda_str, sizeof(s_connected_bda_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        /* Find device name if known in cache */
        s_connected_name[0] = '\0';
        for (int i = 0; i < s_device_count; i++) {
            if (memcmp(s_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
                strncpy(s_connected_name, s_devices[i].name, sizeof(s_connected_name) - 1);
                break;
            }
        }
        if (s_connected_name[0] == '\0') {
            strncpy(s_connected_name, "Connecting Device...", sizeof(s_connected_name) - 1);
        }

        xSemaphoreGive(s_bt_lock);
    }

    ESP_LOGI(TAG, "Attempting A2DP Source connection to %02x:%02x:%02x:%02x:%02x:%02x...",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

    esp_err_t err = esp_a2d_source_connect((uint8_t *)bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_source_connect failed: %s (0x%x)", esp_err_to_name(err), err);
        if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_a2d_state = BT_MGR_A2D_STATE_DISCONNECTED;
            xSemaphoreGive(s_bt_lock);
        }
    }
    return err;
}

esp_err_t bt_manager_connect_str(const char *mac_str)
{
    if (!mac_str || strlen(mac_str) < 12) {
        return ESP_ERR_INVALID_ARG;
    }

    /* URL-decode if %3A or %3a exists */
    char clean[64] = {0};
    int d = 0;
    const char *s = mac_str;
    while (*s && d < sizeof(clean) - 1) {
        if (*s == '%' && isxdigit((int)*(s + 1)) && isxdigit((int)*(s + 2))) {
            char hex[3] = { *(s + 1), *(s + 2), '\0' };
            clean[d++] = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == ' ' || *s == '"' || *s == '\'') {
            s++;
        } else {
            clean[d++] = *s++;
        }
    }
    clean[d] = '\0';

    int mac[6];
    if (sscanf(clean, "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6 ||
        sscanf(clean, "%02x-%02x-%02x-%02x-%02x-%02x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6 ||
        sscanf(clean, "%02x%02x%02x%02x%02x%02x",       &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        esp_bd_addr_t bda;
        for (int i = 0; i < 6; i++) {
            bda[i] = (uint8_t)mac[i];
        }
        return bt_manager_connect(bda);
    }

    ESP_LOGE(TAG, "Cannot parse MAC address string: '%s' (cleaned: '%s')", mac_str, clean);
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

extern bool bta_av_co_get_active_codec_info(char *codec_name, size_t max_name_len, uint32_t *rate, uint32_t *bitrate_kbps, uint8_t *bitpool);

void bt_manager_get_status(bt_mgr_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status->connected_name, 0, sizeof(out_status->connected_name));
    memset(out_status->connected_bda_str, 0, sizeof(out_status->connected_bda_str));
    memset(out_status->codec_name, 0, sizeof(out_status->codec_name));
    out_status->sample_rate = 0;
    out_status->bitrate_kbps = 0;
    out_status->bitpool = 0;

    if (xSemaphoreTake(s_bt_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        out_status->a2d_state = s_a2d_state;
        out_status->audio_state = s_audio_state;
        out_status->is_scanning = s_is_scanning;
        strncpy(out_status->connected_name, s_connected_name, sizeof(out_status->connected_name) - 1);
        strncpy(out_status->connected_bda_str, s_connected_bda_str, sizeof(out_status->connected_bda_str) - 1);
        out_status->discovered_count = s_device_count;

        if (s_a2d_state == BT_MGR_A2D_STATE_CONNECTED) {
            bta_av_co_get_active_codec_info(out_status->codec_name, sizeof(out_status->codec_name),
                                           &out_status->sample_rate, &out_status->bitrate_kbps,
                                           &out_status->bitpool);
        } else {
            strncpy(out_status->codec_name, "None", sizeof(out_status->codec_name) - 1);
        }
        xSemaphoreGive(s_bt_lock);
    } else {
        out_status->a2d_state = s_a2d_state;
        out_status->audio_state = s_audio_state;
        out_status->is_scanning = s_is_scanning;
        out_status->discovered_count = 0;
        strncpy(out_status->codec_name, "None", sizeof(out_status->codec_name) - 1);
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
