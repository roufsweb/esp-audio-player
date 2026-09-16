#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "sd_card.h"
#include "audio_player.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "bt_manager.h"

static const char *TAG = "A2DP_SRC";


/* Audio data callback - pulls decoded PCM from the audio player engine */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
    return audio_player_data_cb(data, len);
}

/* --- CONSOLE COMMAND HANDLERS --- */
static int cmd_scan(int argc, char **argv) {
    ESP_LOGI(TAG, "Initiating scan for nearby Bluetooth devices...");
    bt_manager_start_scan(10);
    return 0;
}

static int cmd_connect(int argc, char **argv) {
    if (argc < 2) {
        ESP_LOGE(TAG, "Error: You must provide a MAC address.");
        ESP_LOGE(TAG, "Usage: connect xx:xx:xx:xx:xx:xx");
        return 1;
    }
    esp_err_t err = bt_manager_connect_str(argv[1]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid MAC format or connection failed: %s", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_disconnect(int argc, char **argv) {
    ESP_LOGI(TAG, "Disconnecting from A2DP sink...");
    bt_manager_disconnect();
    return 0;
}

static int cmd_volume(int argc, char **argv) {
    if (argc < 2) {
        printf("Current volume: %u%%\n", audio_player_get_volume());
        return 0;
    }
    int vol = atoi(argv[1]);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    audio_player_set_volume((uint8_t)vol);
    printf("Volume set to %d%%\n", vol);
    return 0;
}

static int cmd_play(int argc, char **argv) {
    audio_player_play();
    printf("Playback resumed.\n");
    return 0;
}

static int cmd_pause(int argc, char **argv) {
    audio_player_pause();
    printf("Playback paused.\n");
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    audio_player_stop();
    printf("Playback stopped.\n");
    return 0;
}

static int cmd_play_file(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: play_file <path> (e.g. play_file /sdcard/track1.wav)\n");
        return 1;
    }
    esp_err_t ret = audio_player_play_file(argv[1]);
    if (ret != ESP_OK) {
        printf("Failed to play file '%s': %s\n", argv[1], esp_err_to_name(ret));
        return 1;
    }
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    printf("Started playback: %s\n", argv[1]);
    return 0;
}

static int cmd_tone(int argc, char **argv) {
    double freq = 440.0;
    if (argc >= 2) {
        freq = atof(argv[1]);
    }
    audio_player_set_tone(freq);
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    printf("Playing test tone: %.1f Hz\n", freq);
    return 0;
}

static int cmd_status(int argc, char **argv) {
    audio_player_print_status();
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    printf("Rebooting ESP32...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

void register_console_commands(void)
{
    esp_console_cmd_t scan_cmd = {
        .command = "scan",
        .help = "Scan for nearby Bluetooth devices",
        .hint = NULL,
        .func = &cmd_scan,
    };
    esp_console_cmd_register(&scan_cmd);

    esp_console_cmd_t connect_cmd = {
        .command = "connect",
        .help = "Connect to an A2DP Sink device",
        .hint = "<mac_address>",
        .func = &cmd_connect,
    };
    esp_console_cmd_register(&connect_cmd);

    esp_console_cmd_t disconnect_cmd = {
        .command = "disconnect",
        .help = "Disconnect from an A2DP Sink device",
        .hint = "<mac_address>",
        .func = &cmd_disconnect,
    };
    esp_console_cmd_register(&disconnect_cmd);

    esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "Resume audio playback",
        .hint = NULL,
        .func = &cmd_play,
    };
    esp_console_cmd_register(&play_cmd);

    esp_console_cmd_t pause_cmd = {
        .command = "pause",
        .help = "Pause audio playback",
        .hint = NULL,
        .func = &cmd_pause,
    };
    esp_console_cmd_register(&pause_cmd);

    esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "Stop current audio playback",
        .hint = NULL,
        .func = &cmd_stop,
    };
    esp_console_cmd_register(&stop_cmd);

    esp_console_cmd_t play_file_cmd = {
        .command = "play_file",
        .help = "Play an uncompressed WAV audio file from SD card",
        .hint = "<path>",
        .func = &cmd_play_file,
    };
    esp_console_cmd_register(&play_file_cmd);

    esp_console_cmd_t tone_cmd = {
        .command = "tone",
        .help = "Generate a mathematical sine test tone (e.g. tone 440 or tone 1000)",
        .hint = "[frequency_hz]",
        .func = &cmd_tone,
    };
    esp_console_cmd_register(&tone_cmd);

    esp_console_cmd_t volume_cmd = {
        .command = "volume",
        .help = "Get or set digital volume (0-100%)",
        .hint = "[percentage]",
        .func = &cmd_volume,
    };
    esp_console_cmd_register(&volume_cmd);

    esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Display current audio playback status and progress",
        .hint = NULL,
        .func = &cmd_status,
    };
    esp_console_cmd_register(&status_cmd);

    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Software reboot the ESP32",
        .hint = NULL,
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);
}

static void main_media_ctrl_cb(audio_player_cmd_t cmd)
{
    if (cmd == AUDIO_PLAYER_CMD_START) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    } else if (cmd == AUDIO_PLAYER_CMD_SUSPEND) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    } else if (cmd == AUDIO_PLAYER_CMD_STOP) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
    }
}

static void on_wifi_state_change(bool connected, const char *ip_str)
{
    if (connected) {
        ESP_LOGI(TAG, "Wi-Fi link established! Starting Web Dashboard at http://%s...", ip_str);
        web_server_start();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing ESP Audio Player (A2DP Source)...");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize Audio Player Engine */
    ESP_ERROR_CHECK(audio_player_init());
    audio_player_set_media_ctrl_cb(main_media_ctrl_cb);

    /* Test and verify external PSRAM */
    size_t psram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "External PSRAM free: %lu bytes (%lu KB)", (unsigned long)psram_size, (unsigned long)(psram_size / 1024));
    if (psram_size > 0) {
        void *test_ptr = heap_caps_malloc(1024 * 1024, MALLOC_CAP_SPIRAM);
        if (test_ptr != NULL) {
            memset(test_ptr, 0x5A, 1024 * 1024);
            ESP_LOGI(TAG, "PSRAM 1 MB self-test allocation: SUCCESS");
            free(test_ptr);
        } else {
            ESP_LOGW(TAG, "PSRAM 1 MB self-test allocation failed");
        }
    } else {
        ESP_LOGW(TAG, "No PSRAM detected by heap allocator");
    }

    /* Initialize Bluetooth controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    /* Initialize Bluedroid stack */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Initialize Bluetooth Manager */
    ESP_ERROR_CHECK(bt_manager_init());

    /* Register GAP callback */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_manager_gap_cb));

    /* Initialize A2DP Source */
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_manager_a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_app_a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    /* Set device name and discoverability */
    esp_bt_gap_set_device_name("ESP32_A2DP_SRC_XQ");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* Initialize 1-bit SDMMC storage (MicroSD slot) */
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not detected or mount failed (%s). Playback from SD is unavailable until inserted.", esp_err_to_name(sd_ret));
    }

    /* Initialize Wi-Fi subsystem (SSID: rouf.iot) */
    wifi_manager_register_state_callback(on_wifi_state_change);
    esp_err_t wifi_ret = wifi_manager_init();
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_ret));
    }

    /* Initialize Interactive Console */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32>";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    register_console_commands();
    sd_card_register_console_commands();
    wifi_manager_register_console_commands();
    
    ESP_LOGI(TAG, "Initialization complete. Type 'help' for a list of commands.");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}


