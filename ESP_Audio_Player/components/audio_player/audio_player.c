#include "audio_player.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

/* High-performance dr_flac with zero CRC overhead and lean memory profile */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_CRC                  /* Eliminate software CRC checks for 30-50% CPU boost */
#define DR_FLAC_NO_SIMD                 /* Xtensa LX6 has no x86/ARM SIMD */
#define DR_FLAC_NO_PICTURE_METADATA_MALLOC /* Never allocate RAM for embedded album art */
#define DR_FLAC_BUFFER_SIZE 4096        /* 4 KB stream buffer */
#include "dr_flac.h"

static const char *TAG = "AUDIO_PLAYER";

#define DEFAULT_SAMPLE_RATE 44100
#define DEFAULT_TONE_FREQ   440.0

/* 512 KB PSRAM Ring Buffer: ~2.97 seconds of 44.1kHz 16-bit stereo audio */
#define PCM_RING_BUF_SIZE   (512 * 1024)
#define PCM_REFILL_THRESH   (256 * 1024)            /* Refill when buffer dips below ~1.5 seconds */
#define PCM_HIGH_WATERMARK  (384 * 1024)            /* Fill up to ~2.2 seconds, then sleep */
#define PCM_CHUNK_FRAMES    1024                    /* 1024 stereo frames per decode iteration */
#define PCM_CHUNK_BYTES     (PCM_CHUNK_FRAMES * 4)  /* 4096 bytes (4 KB) */

static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_ring_lock = NULL;
static SemaphoreHandle_t s_decode_sem = NULL;
static TaskHandle_t      s_decode_task = NULL;

static uint8_t          *s_ring_buf = NULL;
static int16_t          *s_mono_tmp = NULL;

static uint32_t          s_ring_write = 0;
static uint32_t          s_ring_read = 0;
static uint32_t          s_ring_filled = 0;
static volatile bool     s_decode_eof = false;

static audio_player_state_t s_state = AUDIO_STATE_STOPPED;
static audio_player_source_t s_source = AUDIO_SOURCE_SINE;
static audio_player_media_ctrl_cb_t s_media_ctrl_cb = NULL;
static uint8_t s_volume = 100;

/* Active audio path */
static char s_audio_path[256] = {0};

/* WAV file playback state */
static FILE *s_wav_file = NULL;
static uint32_t s_wav_sample_rate = DEFAULT_SAMPLE_RATE;
static uint16_t s_wav_channels = 2;
static uint16_t s_wav_bit_depth = 16;
static uint32_t s_wav_data_offset = 0;
static uint32_t s_wav_total_bytes = 0;
static uint32_t s_wav_played_bytes = 0;

/* FLAC file playback state */
static drflac *s_flac = NULL;
static uint32_t s_flac_sample_rate = DEFAULT_SAMPLE_RATE;
static uint16_t s_flac_channels = 2;
static uint16_t s_flac_bit_depth = 16;
static uint32_t s_flac_total_bytes = 0;
static uint32_t s_flac_played_bytes = 0;

/* Sine wave state */
static double s_sine_phase = 0.0;
static double s_sine_freq = DEFAULT_TONE_FREQ;

/* PSRAM allocation callbacks for dr_flac to protect internal SRAM */
static void *flac_malloc(size_t sz, void *pUserData)
{
    (void)pUserData;
    void *ptr = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(sz);
    }
    return ptr;
}

static void *flac_realloc(void *p, size_t sz, void *pUserData)
{
    (void)pUserData;
    void *ptr = heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = realloc(p, sz);
    }
    return ptr;
}

static void flac_free(void *p, void *pUserData)
{
    (void)pUserData;
    free(p);
}

static drflac_allocation_callbacks s_flac_alloc = {
    .pUserData = NULL,
    .onMalloc = flac_malloc,
    .onRealloc = flac_realloc,
    .onFree = flac_free,
};

static void close_active_file(void)
{
    if (s_wav_file != NULL) {
        fclose(s_wav_file);
        s_wav_file = NULL;
    }
    if (s_flac != NULL) {
        drflac_close(s_flac);
        s_flac = NULL;
    }
}

static bool has_extension(const char *path, const char *ext)
{
    if (!path || !ext) return false;
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    const char *p = path + plen - elen;
    while (*p && *ext) {
        if (tolower((int)*p) != tolower((int)*ext)) return false;
        p++;
        ext++;
    }
    return true;
}

void audio_player_set_media_ctrl_cb(audio_player_media_ctrl_cb_t cb)
{
    s_media_ctrl_cb = cb;
}

void audio_player_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;
    s_volume = volume_pct;
    ESP_LOGI(TAG, "Audio player digital volume set to %d%%", s_volume);
}

uint8_t audio_player_get_volume(void)
{
    return s_volume;
}

/* Ring buffer signaling helper */
static inline void notify_decode_task(void)
{
    if (!s_decode_sem) return;
    if (xPortInIsrContext()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(s_decode_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    } else {
        xSemaphoreGive(s_decode_sem);
    }
}

/* Query bytes buffered in ring buffer */
static uint32_t ring_available(void)
{
    uint32_t filled = 0;
    if (s_ring_lock && xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        filled = s_ring_filled;
        xSemaphoreGive(s_ring_lock);
    }
    return filled;
}

/* Reset ring buffer pointers and counters */
static void ring_flush(void)
{
    if (s_ring_lock && xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ring_write = 0;
        s_ring_read = 0;
        s_ring_filled = 0;
        s_decode_eof = false;
        xSemaphoreGive(s_ring_lock);
    }
}

/* Write PCM data into circular ring buffer */
static uint32_t ring_write(const uint8_t *src, uint32_t len)
{
    if (!s_ring_buf || !s_ring_lock || len == 0 || !src) return 0;

    if (xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }

    uint32_t space = PCM_RING_BUF_SIZE - s_ring_filled;
    if (len > space) {
        len = space;
    }
    if (len == 0) {
        xSemaphoreGive(s_ring_lock);
        return 0;
    }

    uint32_t first_part = PCM_RING_BUF_SIZE - s_ring_write;
    if (first_part > len) {
        first_part = len;
    }
    memcpy(s_ring_buf + s_ring_write, src, first_part);

    uint32_t second_part = len - first_part;
    if (second_part > 0) {
        memcpy(s_ring_buf, src + first_part, second_part);
        s_ring_write = second_part;
    } else {
        s_ring_write += first_part;
        if (s_ring_write >= PCM_RING_BUF_SIZE) {
            s_ring_write = 0;
        }
    }

    s_ring_filled += len;
    xSemaphoreGive(s_ring_lock);
    return len;
}

/* Read PCM data from circular ring buffer */
static uint32_t ring_read(uint8_t *dst, uint32_t len)
{
    if (!s_ring_buf || !s_ring_lock || len == 0 || !dst) return 0;

    if (xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    uint32_t avail = s_ring_filled;
    if (avail > len) {
        avail = len;
    }
    if (avail == 0) {
        xSemaphoreGive(s_ring_lock);
        return 0;
    }

    uint32_t first_part = PCM_RING_BUF_SIZE - s_ring_read;
    if (first_part > avail) {
        first_part = avail;
    }
    memcpy(dst, s_ring_buf + s_ring_read, first_part);

    uint32_t second_part = avail - first_part;
    if (second_part > 0) {
        memcpy(dst + first_part, s_ring_buf, second_part);
        s_ring_read = second_part;
    } else {
        s_ring_read += first_part;
        if (s_ring_read >= PCM_RING_BUF_SIZE) {
            s_ring_read = 0;
        }
    }

    s_ring_filled -= avail;
    xSemaphoreGive(s_ring_lock);
    return avail;
}

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
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "WAV header incomplete (fmt: %d, data: %d)", found_fmt, found_data);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/* Decode one chunk into 16-bit 44.1kHz stereo PCM. Caller MUST hold s_lock. */
static uint32_t decode_chunk(uint8_t *out_buf, uint32_t max_bytes)
{
    if (s_state != AUDIO_STATE_PLAYING || s_decode_eof) {
        return 0;
    }

    if (s_source == AUDIO_SOURCE_FLAC && s_flac != NULL) {
        if (s_flac_channels == 2) {
            drflac_uint64 frames_needed = max_bytes / 4;
            drflac_uint64 frames_read = drflac_read_pcm_frames_s16(s_flac, frames_needed, (drflac_int16 *)out_buf);
            uint32_t bytes_read = (uint32_t)(frames_read * 4);
            s_flac_played_bytes += bytes_read;

            if (frames_read < frames_needed) {
                ESP_LOGI(TAG, "FLAC decode reached end of file (%lu / %lu bytes)",
                         (unsigned long)s_flac_played_bytes, (unsigned long)s_flac_total_bytes);
                s_decode_eof = true;
                close_active_file();
            }
            return bytes_read;
        } else if (s_flac_channels == 1) {
            uint32_t frames_needed = max_bytes / 4;
            if (frames_needed > PCM_CHUNK_FRAMES) frames_needed = PCM_CHUNK_FRAMES;

            drflac_uint64 frames_read = drflac_read_pcm_frames_s16(s_flac, frames_needed, (drflac_int16 *)s_mono_tmp);
            s_flac_played_bytes += (uint32_t)(frames_read * 2);

            int16_t *stereo = (int16_t *)out_buf;
            for (uint32_t i = 0; i < (uint32_t)frames_read; i++) {
                stereo[i * 2]     = s_mono_tmp[i];
                stereo[i * 2 + 1] = s_mono_tmp[i];
            }

            if (frames_read < (drflac_uint64)frames_needed) {
                ESP_LOGI(TAG, "Mono FLAC decode reached end of file");
                s_decode_eof = true;
                close_active_file();
            }
            return (uint32_t)(frames_read * 4);
        }
    } else if (s_source == AUDIO_SOURCE_WAV && s_wav_file != NULL) {
        if (s_wav_channels == 2) {
            size_t bytes_read = fread(out_buf, 1, max_bytes, s_wav_file);
            s_wav_played_bytes += bytes_read;

            if (bytes_read < max_bytes) {
                ESP_LOGI(TAG, "WAV decode reached end of file (%lu / %lu bytes)",
                         (unsigned long)s_wav_played_bytes, (unsigned long)s_wav_total_bytes);
                s_decode_eof = true;
                close_active_file();
            }
            return (uint32_t)bytes_read;
        } else if (s_wav_channels == 1) {
            uint32_t frames_needed = max_bytes / 4;
            if (frames_needed > PCM_CHUNK_FRAMES) frames_needed = PCM_CHUNK_FRAMES;
            size_t mono_bytes = frames_needed * 2;

            size_t bytes_read = fread(s_mono_tmp, 1, mono_bytes, s_wav_file);
            s_wav_played_bytes += bytes_read;

            uint32_t samples_read = bytes_read / 2;
            int16_t *stereo = (int16_t *)out_buf;
            for (uint32_t i = 0; i < samples_read; i++) {
                stereo[i * 2]     = s_mono_tmp[i];
                stereo[i * 2 + 1] = s_mono_tmp[i];
            }

            if (bytes_read < mono_bytes) {
                ESP_LOGI(TAG, "Mono WAV decode reached end of file");
                s_decode_eof = true;
                close_active_file();
            }
            return samples_read * 4;
        }
    }

    return 0;
}

/* Dedicated background audio decode task running on Core 1 */
static void audio_decode_task(void *arg)
{
    ESP_LOGI(TAG, "Audio decode task running on Core %d at priority %d", xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk = (uint8_t *)malloc(PCM_CHUNK_BYTES);
    }
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate decode task chunk buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* Sleep until signaled or 50ms periodic check */
        xSemaphoreTake(s_decode_sem, pdMS_TO_TICKS(50));

        while (1) {
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
                break;
            }

            if (s_state != AUDIO_STATE_PLAYING || s_decode_eof) {
                xSemaphoreGive(s_lock);
                break;
            }

            uint32_t filled = ring_available();
            if (filled >= PCM_HIGH_WATERMARK) {
                /* Buffer is full enough (~2.2s of audio), sleep until drained */
                xSemaphoreGive(s_lock);
                break;
            }

            uint32_t space = PCM_RING_BUF_SIZE - filled;
            uint32_t to_decode = (space < PCM_CHUNK_BYTES) ? space : PCM_CHUNK_BYTES;
            to_decode &= ~3U; /* Align to stereo 16-bit frame */
            if (to_decode == 0) {
                xSemaphoreGive(s_lock);
                break;
            }

            uint32_t decoded = decode_chunk(chunk, to_decode);
            xSemaphoreGive(s_lock);

            if (decoded == 0) {
                break;
            }

            ring_write(chunk, decoded);

            /* Yield 1 tick (10 ms) every 4 chunks (~16 KB = ~93 ms audio) to feed Core 1 Task Watchdog.
             * CONFIG_FREERTOS_HZ is 100 Hz, so pdMS_TO_TICKS(2) evaluated to 0 ticks (no-op).
             * Yielding 1 tick every 4 chunks produces ~93 ms of audio every ~13 ms (7x real-time),
             * while allowing the Core 1 IDLE task to regularly feed the watchdog. */
            static uint32_t s_chunk_count = 0;
            if ((++s_chunk_count & 3) == 0) {
                vTaskDelay(1);
            }
        }
    }
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
    if (s_ring_lock == NULL) {
        s_ring_lock = xSemaphoreCreateMutex();
        if (s_ring_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create ring buffer mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_decode_sem == NULL) {
        s_decode_sem = xSemaphoreCreateBinary();
        if (s_decode_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create decode semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Allocate 512 KB ring buffer in PSRAM */
    if (s_ring_buf == NULL) {
        s_ring_buf = (uint8_t *)heap_caps_malloc(PCM_RING_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring_buf) {
            ESP_LOGW(TAG, "Failed to allocate 512KB ring buffer in PSRAM, falling back to 128KB");
            s_ring_buf = (uint8_t *)heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_ring_buf) {
            ESP_LOGE(TAG, "Failed to allocate audio ring buffer");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Allocated %d KB audio ring buffer in %s (~%.1f sec buffer)",
                 PCM_RING_BUF_SIZE / 1024,
                 esp_ptr_external_ram(s_ring_buf) ? "PSRAM" : "internal RAM",
                 (double)PCM_RING_BUF_SIZE / (DEFAULT_SAMPLE_RATE * 4.0));
    }

    /* Allocate mono conversion buffer */
    if (s_mono_tmp == NULL) {
        s_mono_tmp = (int16_t *)heap_caps_malloc(PCM_CHUNK_FRAMES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_mono_tmp) {
            s_mono_tmp = (int16_t *)malloc(PCM_CHUNK_FRAMES * sizeof(int16_t));
        }
    }

    ring_flush();

    /* Pin decode task to Core 1 at priority 5.
     * 8 KB stack provides plenty of headroom for dr_flac (< 1.5 KB stack usage)
     * while preserving critical internal DRAM needed for Bluetooth and Wi-Fi. */
    if (s_decode_task == NULL) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            audio_decode_task,
            "audio_decode",
            8192,               /* 8 KB stack: ample headroom for dr_flac while protecting DRAM */
            NULL,
            5,                  /* Priority 5: above idle, cooperates with system */
            &s_decode_task,
            1                   /* Pinned to Core 1 (APP_CPU) */
        );
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio decode task on Core 1");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Decode task created with 8 KB stack on Core 1");
    }

    s_state = AUDIO_STATE_STOPPED;
    s_source = AUDIO_SOURCE_SINE;
    s_sine_freq = DEFAULT_TONE_FREQ;
    s_sine_phase = 0.0;
    ESP_LOGI(TAG, "Audio player engine fully initialized (Core 1 pinned, 512KB PSRAM buffer).");
    return ESP_OK;
}

/* Fast non-blocking A2DP data callback (Core 0 BT stack) - pure memcpy from ring buffer */
int32_t audio_player_data_cb(uint8_t *data, int32_t len)
{
    if (len <= 0 || data == NULL) {
        return 0;
    }

    if (s_state != AUDIO_STATE_PLAYING) {
        memset(data, 0, len);
        return len;
    }

    if (s_source == AUDIO_SOURCE_SINE) {
        int16_t *pcm = (int16_t *)data;
        int samples = len / 4;
        for (int i = 0; i < samples; i++) {
            double sin_val = sin(s_sine_phase);
            int16_t val = (int16_t)(sin_val * 16383.0);
            pcm[i * 2]     = val;
            pcm[i * 2 + 1] = val;

            s_sine_phase += 2.0 * M_PI * s_sine_freq / DEFAULT_SAMPLE_RATE;
            if (s_sine_phase >= 2.0 * M_PI) {
                s_sine_phase -= 2.0 * M_PI;
            }
        }
    } else if (s_source == AUDIO_SOURCE_WAV || s_source == AUDIO_SOURCE_FLAC) {
        uint32_t read_bytes = ring_read(data, len);

        if (read_bytes < (uint32_t)len) {
            /* Buffer underrun or end of stream: pad remainder with silence */
            memset(data + read_bytes, 0, len - read_bytes);

            if (s_decode_eof && read_bytes == 0) {
                ESP_LOGI(TAG, "Audio playback reached end of buffered stream.");
                s_state = AUDIO_STATE_STOPPED;
                s_decode_eof = false;
                close_active_file();
            }
        }

        /* Wake decode task whenever buffer drains below 256 KB threshold.
         * Use a direct read of s_ring_filled (32-bit aligned, Xtensa is
         * atomic for aligned 32-bit loads) to avoid taking s_ring_lock
         * inside the latency-critical BT data callback. */
        if (s_ring_filled < PCM_REFILL_THRESH && !s_decode_eof) {
            notify_decode_task();
        }
    } else {
        memset(data, 0, len);
    }

    /* Apply digital volume scaling */
    if (s_volume < 100) {
        int16_t *samples = (int16_t *)data;
        int num_samples = len / 2;
        int32_t vol = s_volume;
        for (int i = 0; i < num_samples; i++) {
            samples[i] = (int16_t)((samples[i] * vol) / 100);
        }
    }

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

    /* Close previously open file and flush ring buffer */
    s_state = AUDIO_STATE_STOPPED;
    close_active_file();
    ring_flush();

    /* Container auto-detection: check extension or magic bytes */
    bool is_flac = has_extension(path, ".flac");
    bool is_wav = has_extension(path, ".wav");

    if (!is_flac && !is_wav) {
        FILE *probe = fopen(path, "rb");
        if (probe) {
            char magic[4] = {0};
            if (fread(magic, 1, 4, probe) == 4) {
                if (strncmp(magic, "fLaC", 4) == 0) is_flac = true;
                else if (strncmp(magic, "RIFF", 4) == 0) is_wav = true;
            }
            fclose(probe);
        }
    }

    if (is_flac) {
        drflac *flac = drflac_open_file(path, &s_flac_alloc);
        if (!flac) {
            ESP_LOGE(TAG, "Cannot decode FLAC file '%s'", path);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }

        if (flac->channels > 2 || flac->channels == 0) {
            ESP_LOGE(TAG, "Unsupported FLAC channels (%u). Only Mono and Stereo supported.", flac->channels);
            drflac_close(flac);
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_SUPPORTED;
        }

        s_flac = flac;
        s_flac_sample_rate = flac->sampleRate;
        s_flac_channels = flac->channels;
        s_flac_bit_depth = flac->bitsPerSample;
        s_flac_total_bytes = (uint32_t)(flac->totalPCMFrameCount * flac->channels * 2);
        s_flac_played_bytes = 0;

        strncpy(s_audio_path, path, sizeof(s_audio_path) - 1);
        s_source = AUDIO_SOURCE_FLAC;
        s_state = AUDIO_STATE_PLAYING;
        s_decode_eof = false;

        double duration_sec = flac->sampleRate > 0 ? (double)flac->totalPCMFrameCount / flac->sampleRate : 0.0;
        ESP_LOGI(TAG, "Playing FLAC: '%s'", path);
        ESP_LOGI(TAG, "Format: %lu Hz, %u-bit, %s (Duration: %.2f sec, Frames: %llu)",
                 (unsigned long)s_flac_sample_rate, s_flac_bit_depth,
                 s_flac_channels == 2 ? "Stereo" : "Mono",
                 duration_sec, (unsigned long long)flac->totalPCMFrameCount);

        if (s_flac_sample_rate != 44100) {
            ESP_LOGW(TAG, "FLAC sample rate is %lu Hz. Target is 44100 Hz. Pitch may be shifted without SRC.", (unsigned long)s_flac_sample_rate);
        }
    } else {
        /* Process WAV file container */
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
        strncpy(s_audio_path, path, sizeof(s_audio_path) - 1);
        s_wav_data_offset = data_offset;
        s_wav_total_bytes = data_len;
        s_wav_played_bytes = 0;

        s_source = AUDIO_SOURCE_WAV;
        s_state = AUDIO_STATE_PLAYING;
        s_decode_eof = false;

        double duration_sec = (double)data_len / (s_wav_sample_rate * s_wav_channels * 2);
        ESP_LOGI(TAG, "Playing WAV: '%s'", path);
        ESP_LOGI(TAG, "Format: %lu Hz, %u-bit, %s (Duration: %.2f sec, Payload: %lu KB)",
                 (unsigned long)s_wav_sample_rate, s_wav_bit_depth,
                 s_wav_channels == 2 ? "Stereo" : "Mono",
                 duration_sec, (unsigned long)(data_len / 1024));
    }

    /* Release lock so the Core 1 decode task can immediately begin filling the ring buffer */
    xSemaphoreGive(s_lock);

    /* Signal the dedicated decode task on Core 1 (32 KB stack) to start decoding */
    notify_decode_task();

    /* Wait briefly (up to 300 ms) for Core 1 decode task to pre-buffer at least ~64 KB of audio (or EOF).
     * ALL decoding executes safely on Core 1's dedicated 32 KB task stack.
     * The caller task (HTTP server / console) consumes ZERO stack for decoding, completely
     * eliminating the stack-overflow reboot panic when clicking Play in the Web UI. */
    for (int i = 0; i < 30; i++) {
        if (ring_available() >= (64 * 1024) || s_decode_eof) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Pre-buffered %lu bytes before starting A2DP stream", (unsigned long)ring_available());

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

    close_active_file();
    ring_flush();

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
    notify_decode_task();
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
    close_active_file();
    ring_flush();
    s_state = AUDIO_STATE_STOPPED;
    s_wav_played_bytes = 0;
    s_flac_played_bytes = 0;
    ESP_LOGI(TAG, "Audio playback stopped.");
    xSemaphoreGive(s_lock);
    notify_decode_task();
    if (s_media_ctrl_cb) s_media_ctrl_cb(AUDIO_PLAYER_CMD_STOP);
    return ESP_OK;
}

void audio_player_get_status(audio_player_status_t *out_status)
{
    if (!out_status) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        out_status->state = s_state;
        out_status->source = s_source;
        strncpy(out_status->current_path, s_audio_path, sizeof(out_status->current_path) - 1);
        if (s_source == AUDIO_SOURCE_FLAC) {
            out_status->sample_rate = s_flac_sample_rate;
            out_status->channels = s_flac_channels;
            out_status->bit_depth = s_flac_bit_depth;
            out_status->total_audio_bytes = s_flac_total_bytes;
            out_status->played_audio_bytes = s_flac_played_bytes;
        } else {
            out_status->sample_rate = s_wav_sample_rate;
            out_status->channels = s_wav_channels;
            out_status->bit_depth = s_wav_bit_depth;
            out_status->total_audio_bytes = s_wav_total_bytes;
            out_status->played_audio_bytes = s_wav_played_bytes;
        }
        xSemaphoreGive(s_lock);
    }
}

/* --- CONSOLE COMMANDS --- */

static int cmd_play_file(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: play_file <path> (e.g. play_file /sdcard/track1.flac)\n");
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
    else if (st.source == AUDIO_SOURCE_FLAC) src_str = "FLAC_FILE";

    printf("--- Audio Player Status ---\n");
    printf("State:       %s\n", state_str);
    printf("Source:      %s\n", src_str);
    if (st.source == AUDIO_SOURCE_WAV || st.source == AUDIO_SOURCE_FLAC) {
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
        .help = "Play a FLAC or WAV audio file from SD card",
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
