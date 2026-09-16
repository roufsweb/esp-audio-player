#include "audio_player.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_console.h"

static const char *TAG = "AUDIO_PLAYER";

#define DEFAULT_SAMPLE_RATE 44100
#define DEFAULT_TONE_FREQ   440.0

static SemaphoreHandle_t s_lock = NULL;
static audio_player_state_t s_state = AUDIO_STATE_STOPPED;
static audio_player_source_t s_source = AUDIO_SOURCE_SINE;
static audio_player_media_ctrl_cb_t s_media_ctrl_cb = NULL;

void audio_player_set_media_ctrl_cb(audio_player_media_ctrl_cb_t cb)
{
    s_media_ctrl_cb = cb;
}

/* Sine wave state */
static double s_sine_phase = 0.0;
static double s_sine_freq = DEFAULT_TONE_FREQ;

/* WAV file playback state */
static FILE *s_wav_file = NULL;
static char s_wav_path[256] = {0};
static uint32_t s_wav_sample_rate = DEFAULT_SAMPLE_RATE;
static uint16_t s_wav_channels = 2;
static uint16_t s_wav_bit_depth = 16;
static uint32_t s_wav_data_offset = 0;
static uint32_t s_wav_total_bytes = 0;
static uint32_t s_wav_played_bytes = 0;

/* Helper to parse WAV RIFF chunks */
static esp_err_t parse_wav_header(FILE *f, uint32_t *out_data_offset, uint32_t *out_data_len)
{
    char riff[4];
    uint32_t file_size;
    char wave[4];

    if (fread(riff, 1, 4, f) != 4 || strncmp(riff, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid RIFF file");
        return ESP_ERR_INVALID_ARG;
    }

    if (fread(&file_size, 1, 4, f) != 4) return ESP_ERR_INVALID_ARG;
    if (fread(wave, 1, 4, f) != 4 || strncmp(wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid WAVE container");
        return ESP_ERR_INVALID_ARG;
    }

    bool found_fmt = false;
    bool found_data = false;

    while (!found_data && !feof(f)) {
        char chunk_id[4];
        uint32_t chunk_size = 0;

        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 1, 4, f) != 4) break;

        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t audio_format = 0;
            uint16_t num_channels = 0;
            uint32_t sample_rate = 0;
            uint32_t byte_rate = 0;
            uint16_t block_align = 0;
            uint16_t bits_per_sample = 0;

            if (fread(&audio_format, 1, 2, f) != 2) break;
            if (fread(&num_channels, 1, 2, f) != 2) break;
            if (fread(&sample_rate, 1, 4, f) != 4) break;
            if (fread(&byte_rate, 1, 4, f) != 4) break;
            if (fread(&block_align, 1, 2, f) != 2) break;
            if (fread(&bits_per_sample, 1, 2, f) != 2) break;

            if (audio_format != 1) {
                ESP_LOGE(TAG, "Unsupported audio format (%u). Only uncompressed PCM (1) is supported.", audio_format);
                return ESP_ERR_NOT_SUPPORTED;
            }

            if (bits_per_sample != 16) {
                ESP_LOGE(TAG, "Unsupported bit depth (%u-bit). Only 16-bit PCM is supported.", bits_per_sample);
                return ESP_ERR_NOT_SUPPORTED;
            }

            if (sample_rate != 44100) {
                ESP_LOGW(TAG, "Sample rate is %lu Hz. Target is 44100 Hz. Pitch may be shifted without SRC.", (unsigned long)sample_rate);
            }

            s_wav_sample_rate = sample_rate;
            s_wav_channels = num_channels;
            s_wav_bit_depth = bits_per_sample;
            found_fmt = true;

            /* Skip any remaining bytes in fmt chunk if extended */
            int extra = (int)chunk_size - 16;
            if (extra > 0) {
                fseek(f, extra, SEEK_CUR);
            }
        } else if (strncmp(chunk_id, "data", 4) == 0) {
            *out_data_offset = ftell(f);
            *out_data_len = chunk_size;
            found_data = true;
            break;
        } else {
            /* Skip unknown chunk */
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "WAV header incomplete (fmt: %d, data: %d)", found_fmt, found_data);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create audio player mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_state = AUDIO_STATE_STOPPED;
    s_source = AUDIO_SOURCE_SINE;
    s_sine_freq = DEFAULT_TONE_FREQ;
    s_sine_phase = 0.0;
    ESP_LOGI(TAG, "Audio player initialized.");
    return ESP_OK;
}

int32_t audio_player_data_cb(uint8_t *data, int32_t len)
{
    if (len <= 0 || data == NULL) {
        return 0;
    }

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        /* Mutex contention; output silence to prevent glitching */
        memset(data, 0, len);
        return len;
    }

    if (s_state != AUDIO_STATE_PLAYING) {
        memset(data, 0, len);
        xSemaphoreGive(s_lock);
        return len;
    }

    if (s_source == AUDIO_SOURCE_WAV && s_wav_file != NULL) {
        if (s_wav_channels == 2) {
            size_t bytes_read = fread(data, 1, len, s_wav_file);
            s_wav_played_bytes += bytes_read;

            if (bytes_read < (size_t)len) {
                /* End of file reached */
                memset(data + bytes_read, 0, len - bytes_read);
                ESP_LOGI(TAG, "WAV playback reached end of file (%lu / %lu bytes)",
                         (unsigned long)s_wav_played_bytes, (unsigned long)s_wav_total_bytes);
                fclose(s_wav_file);
                s_wav_file = NULL;
                s_state = AUDIO_STATE_STOPPED;
            }
        } else if (s_wav_channels == 1) {
            /* Mono 16-bit: read half into second half of buffer, then expand in place */
            int mono_samples = len / 4;
            int mono_bytes = mono_samples * 2;
            int16_t *mono_buf = (int16_t *)(data + (len / 2));
            size_t bytes_read = fread(mono_buf, 1, mono_bytes, s_wav_file);
            s_wav_played_bytes += bytes_read;

            int16_t *stereo_out = (int16_t *)data;
            int read_samples = bytes_read / 2;
            for (int i = 0; i < read_samples; i++) {
                int16_t s = mono_buf[i];
                stereo_out[i * 2]     = s;
                stereo_out[i * 2 + 1] = s;
            }
            if (bytes_read < (size_t)mono_bytes) {
                /* Zero pad remaining */
                int remaining_samples = mono_samples - read_samples;
                memset(&stereo_out[read_samples * 2], 0, remaining_samples * 4);
                ESP_LOGI(TAG, "Mono WAV playback reached end of file");
                fclose(s_wav_file);
                s_wav_file = NULL;
                s_state = AUDIO_STATE_STOPPED;
            }
        }
    } else if (s_source == AUDIO_SOURCE_SINE) {
        int16_t *pcm = (int16_t *)data;
        int samples = len / 4;
        for (int i = 0; i < samples; i++) {
            double sin_val = sin(s_sine_phase);
            int16_t val = (int16_t)(sin_val * 16383.0); /* Half volume */
            pcm[i * 2]     = val;
            pcm[i * 2 + 1] = val;

            s_sine_phase += 2.0 * M_PI * s_sine_freq / DEFAULT_SAMPLE_RATE;
            if (s_sine_phase >= 2.0 * M_PI) {
                s_sine_phase -= 2.0 * M_PI;
            }
        }
    } else {
        memset(data, 0, len);
    }

    xSemaphoreGive(s_lock);
    return len;
}

esp_err_t audio_player_play_file(const char *path)
{
    if (path == NULL || strlen(path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Close previously open file */
    if (s_wav_file != NULL) {
        fclose(s_wav_file);
        s_wav_file = NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open audio file '%s'", path);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t data_offset = 0;
    uint32_t data_len = 0;
    esp_err_t err = parse_wav_header(f, &data_offset, &data_len);
    if (err != ESP_OK) {
        fclose(f);
        xSemaphoreGive(s_lock);
        return err;
    }

    fseek(f, data_offset, SEEK_SET);

    s_wav_file = f;
    strncpy(s_wav_path, path, sizeof(s_wav_path) - 1);
    s_wav_data_offset = data_offset;
    s_wav_total_bytes = data_len;
    s_wav_played_bytes = 0;

    s_source = AUDIO_SOURCE_WAV;
    s_state = AUDIO_STATE_PLAYING;

    double duration_sec = (double)data_len / (s_wav_sample_rate * s_wav_channels * 2);
    ESP_LOGI(TAG, "Playing WAV: '%s'", path);
    ESP_LOGI(TAG, "Format: %lu Hz, %u-bit, %s (Duration: %.2f sec, Payload: %lu KB)",
             (unsigned long)s_wav_sample_rate, s_wav_bit_depth,
             s_wav_channels == 2 ? "Stereo" : "Mono",
             duration_sec, (unsigned long)(data_len / 1024));

    xSemaphoreGive(s_lock);
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_START);
    return ESP_OK;
}

esp_err_t audio_player_set_tone(double freq_hz)
{
    if (freq_hz <= 20.0 || freq_hz >= 20000.0) {
        freq_hz = DEFAULT_TONE_FREQ;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_wav_file != NULL) {
        fclose(s_wav_file);
        s_wav_file = NULL;
    }

    s_sine_freq = freq_hz;
    s_sine_phase = 0.0;
    s_source = AUDIO_SOURCE_SINE;
    s_state = AUDIO_STATE_PLAYING;

    ESP_LOGI(TAG, "Audio source set to Sine Tone: %.1f Hz", freq_hz);
    xSemaphoreGive(s_lock);
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_START);
    return ESP_OK;
}

esp_err_t audio_player_play(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_state = AUDIO_STATE_PLAYING;
    ESP_LOGI(TAG, "Audio playback resumed.");
    xSemaphoreGive(s_lock);
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_START);
    return ESP_OK;
}

esp_err_t audio_player_pause(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_state = AUDIO_STATE_PAUSED;
    ESP_LOGI(TAG, "Audio playback paused.");
    xSemaphoreGive(s_lock);
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_SUSPEND);
    return ESP_OK;
}

esp_err_t audio_player_stop(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_wav_file != NULL) {
        fclose(s_wav_file);
        s_wav_file = NULL;
    }
    s_state = AUDIO_STATE_STOPPED;
    s_wav_played_bytes = 0;
    ESP_LOGI(TAG, "Audio playback stopped.");
    xSemaphoreGive(s_lock);
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_STOP);
    return ESP_OK;
}

void audio_player_get_status(audio_player_status_t *out_status)
{
    if (!out_status) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        out_status->state = s_state;
        out_status->source = s_source;
        strncpy(out_status->current_path, s_wav_path, sizeof(out_status->current_path));
        out_status->sample_rate = s_wav_sample_rate;
        out_status->channels = s_wav_channels;
        out_status->bit_depth = s_wav_bit_depth;
        out_status->total_audio_bytes = s_wav_total_bytes;
        out_status->played_audio_bytes = s_wav_played_bytes;
        xSemaphoreGive(s_lock);
    }
}

/* --- CONSOLE COMMANDS --- */

static int cmd_play_file(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: play_file <path> (e.g. play_file /sdcard/track1.wav)\n");
        return 1;
    }
    esp_err_t ret = audio_player_play_file(argv[1]);
    if (ret != ESP_OK) {
        printf("Failed to play file '%s': %s\n", argv[1], esp_err_to_name(ret));
        return 1;
    }
    printf("Started playback: %s\n", argv[1]);
    return 0;
}

static int cmd_tone(int argc, char **argv)
{
    double freq = DEFAULT_TONE_FREQ;
    if (argc >= 2) {
        freq = atof(argv[1]);
    }
    audio_player_set_tone(freq);
    printf("Playing test tone: %.1f Hz\n", freq);
    return 0;
}

static int cmd_player_play(int argc, char **argv)
{
    audio_player_play();
    printf("Playback resumed.\n");
    return 0;
}

static int cmd_player_pause(int argc, char **argv)
{
    audio_player_pause();
    printf("Playback paused.\n");
    return 0;
}

static int cmd_player_stop(int argc, char **argv)
{
    audio_player_stop();
    printf("Playback stopped.\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    audio_player_status_t st;
    audio_player_get_status(&st);

    const char *state_str = "STOPPED";
    if (st.state == AUDIO_STATE_PLAYING) state_str = "PLAYING";
    else if (st.state == AUDIO_STATE_PAUSED) state_str = "PAUSED";

    const char *src_str = "NONE";
    if (st.source == AUDIO_SOURCE_SINE) src_str = "SINE_TONE";
    else if (st.source == AUDIO_SOURCE_WAV) src_str = "WAV_FILE";

    printf("--- Audio Player Status ---\n");
    printf("State:       %s\n", state_str);
    printf("Source:      %s\n", src_str);
    if (st.source == AUDIO_SOURCE_WAV) {
        printf("File:        %s\n", st.current_path);
        printf("Sample Rate: %lu Hz\n", (unsigned long)st.sample_rate);
        printf("Channels:    %u (%s)\n", st.channels, st.channels == 2 ? "Stereo" : "Mono");
        printf("Bit Depth:   %u-bit\n", st.bit_depth);
        if (st.total_audio_bytes > 0) {
            float progress = (float)st.played_audio_bytes * 100.0f / (float)st.total_audio_bytes;
            printf("Progress:    %.1f%% (%lu / %lu bytes)\n", progress,
                   (unsigned long)st.played_audio_bytes, (unsigned long)st.total_audio_bytes);
        }
    } else if (st.source == AUDIO_SOURCE_SINE) {
        printf("Frequency:   %.1f Hz\n", s_sine_freq);
    }
    printf("---------------------------\n");
    return 0;
}

void audio_player_print_status(void)
{
    cmd_status(0, NULL);
}


void audio_player_register_console_commands(void)
{
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

    esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "Stop current audio playback and rewind",
        .hint = NULL,
        .func = &cmd_player_stop,
    };
    esp_console_cmd_register(&stop_cmd);

    esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Display current audio playback status and progress",
        .hint = NULL,
        .func = &cmd_status,
    };
    esp_console_cmd_register(&status_cmd);

    /* Override simple play/pause to hook into audio_player engine */
    esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "Resume audio playback",
        .hint = NULL,
        .func = &cmd_player_play,
    };
    esp_console_cmd_register(&play_cmd);

    esp_console_cmd_t pause_cmd = {
        .command = "pause",
        .help = "Pause audio playback",
        .hint = NULL,
        .func = &cmd_player_pause,
    };
    esp_console_cmd_register(&pause_cmd);
}
