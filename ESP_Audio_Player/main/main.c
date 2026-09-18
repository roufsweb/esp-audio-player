#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
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
#include "bt_manager.h"
#include "display_manager.h"
#include "battery.h"

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
    bt_manager_set_volume((uint8_t)vol);
    printf("Volume set to %d%%\n", vol);
    return 0;
}

static int cmd_play(int argc, char **argv) {
    audio_player_play();
    display_manager_update("Playing", "PLAYING", 44100, audio_player_get_volume());
    printf("Playback resumed.\n");
    return 0;
}

static int cmd_pause(int argc, char **argv) {
    audio_player_pause();
    display_manager_update("Paused", "PAUSED", 0, audio_player_get_volume());
    printf("Playback paused.\n");
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    audio_player_stop();
    display_manager_update("Stopped", "STOPPED", 0, audio_player_get_volume());
    printf("Playback stopped.\n");
    return 0;
}

static int cmd_play_file(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: play_file <path> (e.g. play_file /sdcard/track1.wav)\n");
        return 1;
    }
    char full_path[512] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(full_path, " ", sizeof(full_path) - strlen(full_path) - 1);
        strncat(full_path, argv[i], sizeof(full_path) - strlen(full_path) - 1);
    }
    char clean_path[512] = {0};
    const char *p = full_path;
    while (*p == ' ') p++;
    if (*p == '"' || *p == '\'') p++;
    strncpy(clean_path, p, sizeof(clean_path) - 1);
    size_t len = strlen(clean_path);
    while (len > 0 && (clean_path[len - 1] == ' ' || clean_path[len - 1] == '"' || clean_path[len - 1] == '\'')) {
        clean_path[--len] = '\0';
    }

    esp_err_t ret = audio_player_play_file(clean_path);
    if (ret != ESP_OK) {
        printf("Failed to play file '%s': %s\n", clean_path, esp_err_to_name(ret));
        return 1;
    }
    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);
    if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTED) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    }
    display_manager_update(clean_path, "PLAYING", 44100, audio_player_get_volume());
    printf("Started playback: %s\n", clean_path);
    return 0;
}

static int cmd_tone(int argc, char **argv) {
    double freq = 440.0;
    if (argc >= 2) {
        freq = atof(argv[1]);
    }
    audio_player_set_tone(freq);
    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);
    if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTED) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    }
    display_manager_update("Sine Tone Generator", "PLAYING", 44100, audio_player_get_volume());
    printf("Playing test tone: %.1f Hz\n", freq);
    return 0;
}

static int cmd_seek(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: seek <seconds> (e.g. seek 45)\n");
        return 1;
    }
    uint32_t sec = (uint32_t)atoi(argv[1]);
    esp_err_t err = audio_player_seek(sec);
    if (err != ESP_OK) {
        printf("Seek failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Seeked to %lu seconds.\n", (unsigned long)sec);
    return 0;
}

static int cmd_ff(int argc, char **argv) {
    int32_t delta = 10;
    if (argc >= 2) {
        delta = atoi(argv[1]);
        if (delta <= 0) delta = 10;
    }
    esp_err_t err = audio_player_seek_delta(delta);
    if (err != ESP_OK) {
        printf("Fast-forward failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Fast-forwarded +%ld seconds.\n", (long)delta);
    return 0;
}

static int cmd_rew(int argc, char **argv) {
    int32_t delta = 10;
    if (argc >= 2) {
        delta = atoi(argv[1]);
        if (delta <= 0) delta = 10;
    }
    esp_err_t err = audio_player_seek_delta(-delta);
    if (err != ESP_OK) {
        printf("Rewind failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Rewound -%ld seconds.\n", (long)delta);
    return 0;
}

static int cmd_battery(int argc, char **argv) {
    uint32_t mv = battery_get_millivolts();
    uint8_t pct = battery_get_percentage();
    printf("Battery: %u%% (%lu mV, GPIO 34)\n", pct, (unsigned long)mv);
    return 0;
}


static int cmd_status(int argc, char **argv) {
    audio_player_status_t ap_status;
    audio_player_get_status(&ap_status);

    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);

    const char *state_str = "STOPPED";
    if (ap_status.state == AUDIO_STATE_PLAYING) state_str = "PLAYING";
    else if (ap_status.state == AUDIO_STATE_PAUSED) state_str = "PAUSED";

    const char *src_str = "NONE";
    if (ap_status.source == AUDIO_SOURCE_SINE) src_str = "SINE";
    else if (ap_status.source == AUDIO_SOURCE_WAV) src_str = "WAV";
    else if (ap_status.source == AUDIO_SOURCE_FLAC) src_str = "FLAC";

    uint32_t pos_sec = 0;
    uint32_t total_sec = 0;
    if (ap_status.sample_rate > 0 && ap_status.channels > 0) {
        uint32_t bytes_per_sec = (ap_status.source == AUDIO_SOURCE_FLAC)
            ? (ap_status.sample_rate * ap_status.channels * 2)
            : (ap_status.sample_rate * ap_status.channels * (ap_status.bit_depth / 8));
        if (bytes_per_sec > 0) {
            pos_sec = ap_status.played_audio_bytes / bytes_per_sec;
            total_sec = ap_status.total_audio_bytes / bytes_per_sec;
        }
    }

    uint32_t sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t buf_bytes = audio_player_get_buffered_bytes();

    printf("\n=== ESP32 AUDIO PLAYER TELEMETRY ===\n");
    printf("Playback State:  %s\n", state_str);
    printf("Audio Source:    %s\n", src_str);
    if (ap_status.current_path[0] != '\0') {
        printf("Current File:    %s\n", ap_status.current_path);
    }
    if (ap_status.sample_rate > 0) {
        printf("Audio Format:    %lu Hz, %u-bit, %u ch\n",
               (unsigned long)ap_status.sample_rate, (unsigned)ap_status.bit_depth, (unsigned)ap_status.channels);
    }
    if (total_sec > 0) {
        float pct = ((float)ap_status.played_audio_bytes * 100.0f) / (float)ap_status.total_audio_bytes;
        printf("Position:        %02lu:%02lu / %02lu:%02lu (%.1f%%, %lu / %lu bytes)\n",
               (unsigned long)(pos_sec / 60), (unsigned long)(pos_sec % 60),
               (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60),
               pct, (unsigned long)ap_status.played_audio_bytes, (unsigned long)ap_status.total_audio_bytes);
    }
    printf("Ring Buffer:     %lu KB\n", (unsigned long)(buf_bytes / 1024));
    printf("Volume:          %u%%\n", (unsigned)audio_player_get_volume());

    printf("--- Bluetooth Sink (Handshake Confirmed) ---\n");
    const char *bt_state_str = "DISCONNECTED";
    if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTING) bt_state_str = "CONNECTING";
    else if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTED) bt_state_str = "CONNECTED";
    else if (bt_status.a2d_state == BT_MGR_A2D_STATE_DISCONNECTING) bt_state_str = "DISCONNECTING";

    printf("BT State:        %s\n", bt_state_str);
    if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTED) {
        printf("Remote Device:   %s [%s]\n", bt_status.connected_name, bt_status.connected_bda_str);
        printf("Agreed Codec:    %s\n", bt_status.codec_name);
        printf("Agreed Rate:     %lu Hz\n", (unsigned long)bt_status.sample_rate);
        printf("Agreed Bitrate:  %lu kbps (Bitpool: %u)\n",
               (unsigned long)bt_status.bitrate_kbps, (unsigned)bt_status.bitpool);
    }

    printf("--- System Heap ---\n");
    printf("Free SRAM:       %lu KB\n", (unsigned long)(sram_free / 1024));
    printf("Free PSRAM:      %.2f MB\n", (double)psram_free / (1024.0 * 1024.0));
    printf("=====================================\n\n");
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    printf("Rebooting ESP32...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

/* --- JSON OUTPUT COMMANDS FOR WEB SERIAL DASHBOARD --- */

static int cmd_status_json(int argc, char **argv)
{
    audio_player_status_t ap_status;
    audio_player_get_status(&ap_status);

    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);

    const char *state_str = "STOPPED";
    if (ap_status.state == AUDIO_STATE_PLAYING) state_str = "PLAYING";
    else if (ap_status.state == AUDIO_STATE_PAUSED) state_str = "PAUSED";

    const char *src_str = "NONE";
    if (ap_status.source == AUDIO_SOURCE_SINE) src_str = "SINE";
    else if (ap_status.source == AUDIO_SOURCE_WAV) src_str = "WAV";
    else if (ap_status.source == AUDIO_SOURCE_FLAC) src_str = "FLAC";

    const char *bt_state_str = "DISCONNECTED";
    if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTING) bt_state_str = "CONNECTING";
    else if (bt_status.a2d_state == BT_MGR_A2D_STATE_CONNECTED) bt_state_str = "CONNECTED";
    else if (bt_status.a2d_state == BT_MGR_A2D_STATE_DISCONNECTING) bt_state_str = "DISCONNECTING";

    uint32_t pos_sec = 0;
    uint32_t total_sec = 0;
    if (ap_status.sample_rate > 0 && ap_status.channels > 0) {
        uint32_t bytes_per_sec = (ap_status.source == AUDIO_SOURCE_FLAC)
            ? (ap_status.sample_rate * ap_status.channels * 2)
            : (ap_status.sample_rate * ap_status.channels * (ap_status.bit_depth / 8));
        if (bytes_per_sec > 0) {
            pos_sec = ap_status.played_audio_bytes / bytes_per_sec;
            total_sec = ap_status.total_audio_bytes / bytes_per_sec;
        }
    }

    uint32_t sram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t buf_bytes = audio_player_get_buffered_bytes();

    char clean_name[64] = {0};
    for (size_t i = 0; i < sizeof(clean_name) - 1 && bt_status.connected_name[i] != '\0'; i++) {
        char c = bt_status.connected_name[i];
        clean_name[i] = (c >= 32 && c <= 126 && c != '"' && c != '\\') ? c : ' ';
    }

    char clean_mac[24] = {0};
    for (size_t i = 0; i < sizeof(clean_mac) - 1 && bt_status.connected_bda_str[i] != '\0'; i++) {
        char c = bt_status.connected_bda_str[i];
        clean_mac[i] = (c >= 32 && c <= 126 && c != '"' && c != '\\') ? c : '\0';
    }

    char clean_codec[32] = {0};
    for (size_t i = 0; i < sizeof(clean_codec) - 1 && bt_status.codec_name[i] != '\0'; i++) {
        char c = bt_status.codec_name[i];
        clean_codec[i] = (c >= 32 && c <= 126 && c != '"' && c != '\\') ? c : ' ';
    }

    uint8_t batt_pct = battery_get_percentage();
    uint32_t batt_mv = battery_get_millivolts();

    printf("{\"type\":\"status\",\"state\":\"%s\",\"source\":\"%s\",\"file\":\"%s\",\"sample_rate\":%lu,\"bits\":%u,\"channels\":%u,\"pos\":%lu,\"total\":%lu,\"pos_sec\":%lu,\"total_sec\":%lu,\"volume\":%u,\"bt_state\":\"%s\",\"sink_name\":\"%s\",\"sink_mac\":\"%s\",\"bt_codec\":\"%s\",\"bt_bitrate\":%lu,\"bt_bitpool\":%u,\"bt_rate\":%lu,\"scanning\":%s,\"sram\":%lu,\"psram\":%lu,\"buf\":%lu,\"batt_pct\":%u,\"batt_mv\":%lu}\n",
           state_str, src_str, ap_status.current_path,
           (unsigned long)ap_status.sample_rate, (unsigned)ap_status.bit_depth, (unsigned)ap_status.channels,
           (unsigned long)ap_status.played_audio_bytes, (unsigned long)ap_status.total_audio_bytes,
           (unsigned long)pos_sec, (unsigned long)total_sec,
           (unsigned)audio_player_get_volume(),
           bt_state_str, clean_name, clean_mac,
           clean_codec, (unsigned long)bt_status.bitrate_kbps, (unsigned)bt_status.bitpool, (unsigned long)bt_status.sample_rate,
           bt_status.is_scanning ? "true" : "false",
           (unsigned long)sram_free, (unsigned long)psram_free, (unsigned long)buf_bytes,
           (unsigned)batt_pct, (unsigned long)batt_mv);
    return 0;
}

static int cmd_scan_json(int argc, char **argv)
{
    bt_device_entry_t devices[BT_MAX_DISCOVERED_DEVICES];
    size_t count = bt_manager_get_discovered_devices(devices, BT_MAX_DISCOVERED_DEVICES);
    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);

    printf("{\"type\":\"scan\",\"scanning\":%s,\"count\":%u,\"devices\":[",
           bt_status.is_scanning ? "true" : "false", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        char clean_name[64];
        size_t k = 0;
        for (size_t j = 0; devices[i].name[j] != '\0' && k < sizeof(clean_name) - 1; j++) {
            if (devices[i].name[j] == '"' || devices[i].name[j] == '\\') {
                clean_name[k++] = ' ';
            } else {
                clean_name[k++] = devices[i].name[j];
            }
        }
        clean_name[k] = '\0';
        printf("%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
               (i > 0) ? "," : "",
               devices[i].bda_str, clean_name, devices[i].rssi);
    }
    printf("]}\n");
    return 0;
}

static int cmd_ls_json(int argc, char **argv)
{
    const char *target_dir = "/sdcard";
    if (argc >= 2) {
        target_dir = argv[1];
    }
    DIR *dir = opendir(target_dir);
    if (!dir) {
        printf("{\"type\":\"ls\",\"dir\":\"%s\",\"error\":\"open_failed\",\"files\":[]}\n", target_dir);
        return 0;
    }

    printf("{\"type\":\"ls\",\"dir\":\"%s\",\"files\":[", target_dir);
    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full_path[600];
        snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->d_name);
        struct stat st;
        long size = 0;
        bool is_dir = false;
        if (stat(full_path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }
        printf("%s{\"name\":\"%s\",\"path\":\"%s\",\"is_dir\":%s,\"size\":%ld}",
               first ? "" : ",",
               entry->d_name, full_path, is_dir ? "true" : "false", size);
        first = false;
    }
    closedir(dir);
    printf("]}\n");
    return 0;
}

static int cmd_display_test(int argc, char **argv)
{
    printf("Triggering Nokia C1-01 display test pattern...\n");
    display_manager_test_pattern();
    return 0;
}

static int cmd_benchmark_audio(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: benchmark_audio <path_to_audio_file> [seconds]\n");
        printf("Example: benchmark_audio \"/sdcard/Animals - Maroon 5.flac\" 5\n");
        return 1;
    }
    const char *path = argv[1];
    uint32_t seconds = 5;
    if (argc >= 3) {
        seconds = (uint32_t)atoi(argv[2]);
        if (seconds == 0) seconds = 5;
    }
    audio_player_benchmark(path, seconds);
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
        .help = "Play an audio file (FLAC or WAV) from SD card",
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

    esp_console_cmd_t status_json_cmd = {
        .command = "status_json",
        .help = "Output playback and system telemetry as compact JSON",
        .hint = NULL,
        .func = &cmd_status_json,
    };
    esp_console_cmd_register(&status_json_cmd);

    esp_console_cmd_t scan_json_cmd = {
        .command = "scan_json",
        .help = "Output discovered Bluetooth devices as JSON",
        .hint = NULL,
        .func = &cmd_scan_json,
    };
    esp_console_cmd_register(&scan_json_cmd);

    esp_console_cmd_t ls_json_cmd = {
        .command = "ls_json",
        .help = "Output directory listing as JSON",
        .hint = "[path]",
        .func = &cmd_ls_json,
    };
    esp_console_cmd_register(&ls_json_cmd);

    esp_console_cmd_t display_test_cmd = {
        .command = "display_test",
        .help = "Render test pattern on Nokia C1-01 display",
        .hint = NULL,
        .func = &cmd_display_test,
    };
    esp_console_cmd_register(&display_test_cmd);

    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Software reboot the ESP32",
        .hint = NULL,
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    esp_console_cmd_t bench_cmd = {
        .command = "benchmark_audio",
        .help = "Run scientific micro-benchmark isolating SDMMC, FLAC S32/S16 decode, and resamplers",
        .hint = "<path> [seconds]",
        .func = &cmd_benchmark_audio,
    };
    esp_console_cmd_register(&bench_cmd);

    esp_console_cmd_t seek_cmd = {
        .command = "seek",
        .help = "Seek to absolute second in active audio track (e.g. seek 45)",
        .hint = "<seconds>",
        .func = &cmd_seek,
    };
    esp_console_cmd_register(&seek_cmd);

    esp_console_cmd_t ff_cmd = {
        .command = "ff",
        .help = "Fast forward by seconds (default: +10s)",
        .hint = "[seconds]",
        .func = &cmd_ff,
    };
    esp_console_cmd_register(&ff_cmd);

    esp_console_cmd_t rew_cmd = {
        .command = "rew",
        .help = "Rewind by seconds (default: -10s)",
        .hint = "[seconds]",
        .func = &cmd_rew,
    };
    esp_console_cmd_register(&rew_cmd);

    esp_console_cmd_t batt_cmd = {
        .command = "battery",
        .help = "Read battery voltage and percentage on GPIO 34 (ADC1_CH6)",
        .hint = NULL,
        .func = &cmd_battery,
    };
    esp_console_cmd_register(&batt_cmd);
}

static void main_media_ctrl_cb(audio_player_cmd_t cmd)
{
    bt_mgr_status_t bt_status;
    bt_manager_get_status(&bt_status);
    if (bt_status.a2d_state != BT_MGR_A2D_STATE_CONNECTED) {
        return;
    }
    if (cmd == AUDIO_PLAYER_CMD_START) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    } else if (cmd == AUDIO_PLAYER_CMD_SUSPEND) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    } else if (cmd == AUDIO_PLAYER_CMD_STOP) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
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

    /* Initialize Battery Monitor on GPIO 34 */
    battery_init();

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

    /* Initialize Nokia C1-01 Display Subsystem (SPI3 / VSPI: CS=5, SCK=18, MOSI=19, RST=13) */
    esp_err_t disp_ret = display_manager_init();
    if (disp_ret == ESP_OK) {
        ESP_LOGI(TAG, "Nokia C1-01 display driver initialized (CS=GPIO5, SCK=GPIO18, MOSI=GPIO19, RST=GPIO13)");
        display_manager_show_splash();
    } else {
        ESP_LOGW(TAG, "Display manager init failed: %s", esp_err_to_name(disp_ret));
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

    /* Initialize AVRCP Controller before A2DP Source */
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(bt_manager_avrc_ct_cb));

    /* Initialize A2DP Source */
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_manager_a2d_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_app_a2d_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());


    /* Set device name and discoverability */
    esp_bt_gap_set_device_name("ESP32_A2DP_SRC_XQ");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* Initialize 1-bit SDMMC storage (MicroSD slot: CLK=GPIO14, CMD=GPIO15, DAT0=GPIO2) */
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not detected or mount failed (%s). Playback from SD is unavailable until inserted.", esp_err_to_name(sd_ret));
    }

    /* Wi-Fi subsystem disabled:
     * Freeing the single 2.4 GHz RF radio from time-division arbitration conflicts eliminates
     * packet dropouts and stuttering in Bluetooth A2DP audio streaming.
     * All management, playback control, and telemetry are handled via USB Web Serial.
     */
    ESP_LOGI(TAG, "Wi-Fi disabled: 100%% 2.4 GHz RF radio bandwidth dedicated to Bluetooth A2DP audio.");

    /* Initialize Interactive Console with safe 20 KB stack */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "esp32>";
    repl_config.max_cmdline_length = 512;
    repl_config.task_stack_size = 32768;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    register_console_commands();
    sd_card_register_console_commands();
    
    ESP_LOGI(TAG, "Initialization complete. Type 'help' or connect via Web Serial dashboard.");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}


