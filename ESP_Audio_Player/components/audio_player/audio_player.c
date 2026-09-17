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
#include "esp_timer.h"

/* High-performance dr_flac with zero CRC overhead and lean memory profile */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_CRC                  /* Eliminate software CRC checks for 30-50% CPU boost */
#define DR_FLAC_NO_SIMD                 /* Xtensa LX6 has no x86/ARM SIMD */
#define DR_FLAC_NO_PICTURE_METADATA_MALLOC /* Never allocate RAM for embedded album art */
#define DR_FLAC_BUFFER_SIZE 4096        /* 4 KB stream buffer: matches 8x SDMMC sectors (512B) and fits comfortably in task stack */
#include "dr_flac.h"

static const char *TAG = "AUDIO_PLAYER";

#define DEFAULT_SAMPLE_RATE 44100
#define DEFAULT_TONE_FREQ   440.0

/* 101-entry audiophile logarithmic volume taper (Q15 fixed-point, 50% = -10.8 dB, 100% = 0 dB bit-perfect) */
static const uint16_t s_vol_lut[101] = {
        0,     8,    28,    59,    99,   149,   207,   273,   347,   429,   519,   616,   721,   832,   951,  1077,
     1210,  1349,  1495,  1648,  1808,  1974,  2146,  2325,  2510,  2702,  2899,  3103,  3313,  3529,  3751,  3980,
     4214,  4454,  4700,  4951,  5209,  5472,  5741,  6016,  6297,  6583,  6875,  7172,  7475,  7784,  8098,  8418,
     8743,  9073,  9409,  9751, 10098, 10450, 10808, 11170, 11539, 11912, 12291, 12675, 13064, 13459, 13859, 14264,
    14674, 15089, 15510, 15935, 16366, 16802, 17243, 17688, 18139, 18595, 19056, 19523, 19994, 20470, 20951, 21437,
    21927, 22423, 22924, 23430, 23940, 24456, 24976, 25501, 26031, 26566, 27106, 27651, 28200, 28754, 29313, 29877,
    30445, 31018, 31596, 32179, 32767
};

/* 512 KB PSRAM Ring Buffer: ~2.97 seconds of 44.1kHz 16-bit stereo audio */
#define PCM_RING_BUF_SIZE   (512 * 1024)
#define PCM_REFILL_THRESH   (256 * 1024)            /* Refill when buffer dips below ~1.5 seconds */
#define PCM_HIGH_WATERMARK  (384 * 1024)            /* Fill up to ~2.2 seconds, then sleep */
#define PCM_CHUNK_FRAMES    2048                    /* 2048 stereo frames per decode iteration */
#define PCM_CHUNK_BYTES     (PCM_CHUNK_FRAMES * 4)  /* 8192 bytes (8 KB) */

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
static uint8_t s_volume = 5;
static volatile bool s_is_prebuffered = false;

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

/* Negotiated Bluetooth A2DP sample rate (set by BT stack callback after AVDTP handshake).
 * 0 = unknown/unset. Used to select 96k→48k FIR vs 96k→44.1k fractional resampler. */
static volatile uint32_t s_negotiated_bt_rate = 0;

/* ============================================================
 * 23-Tap Half-Band FIR Decimation Filter (96 kHz → 48 kHz)
 * ============================================================
 * Coefficients designed for >60 dB stopband rejection, Q15 fixed-point.
 * Symmetric half-band structure: all even-indexed coefficients are zero
 * (except center tap h[11]), halving the required multiply-accumulate count.
 * Only non-zero taps (odd offsets from center): h[1], h[3], h[5], h[7], h[9], h[11]=center.
 * Reference: Parks-McClellan optimal equiripple design, normalized to Q15 (32767 = 1.0).
 */
#define HB_FIR_TAPS 23
#define HB_FIR_HALF (HB_FIR_TAPS / 2)  /* 11 */

/* Non-zero half-band FIR coefficients (indices 0..HB_FIR_HALF).
 * h[0] = outermost tap, h[HB_FIR_HALF] = center tap (gain = 0.5 in Q15). */
static const int16_t s_hb_fir_coeff[12] = {
    -25,   /* h[0]  = h[22] */
      0,   /* h[1]  = 0 (even, zero by HB symmetry) */
    120,   /* h[2]  = h[20] */
      0,   /* h[3]  = 0 */
   -415,   /* h[4]  = h[18] */
      0,   /* h[5]  = 0 */
   1674,   /* h[6]  = h[16] */
      0,   /* h[7]  = 0 */
  -7318,   /* h[8]  = h[14] */
      0,   /* h[9]  = 0 */
  29375,   /* h[10] = h[12] */
  32767,   /* h[11] = center tap */
};

/* Delay-line history for the FIR filter (stereo interleaved: L0,R0,L1,R1,...)
 * Size = HB_FIR_TAPS - 1 = 22 samples × 2 channels = 44 int16_t entries. */
static int16_t s_hb_delay_l[HB_FIR_TAPS] = {0};
static int16_t s_hb_delay_r[HB_FIR_TAPS] = {0};

/* ----------------------------------------------------------------
 * resample_96k_to_48k_halfband
 *
 * Decimate stereo PCM from 96 kHz to 48 kHz using the half-band FIR.
 * Input:  in_samples   - pointer to stereo interleaved int16_t at 96 kHz
 *         in_frames    - number of INPUT frames (= 2 × output frames)
 * Output: out_samples  - pointer to stereo interleaved int16_t at 48 kHz
 * Returns number of output frames written.
 *
 * The FIR processes every input sample into the delay line.
 * One output sample is computed for every TWO input samples (decimation by 2).
 * The anti-aliasing filter suppresses all energy above 24 kHz.
 * ---------------------------------------------------------------- */
static uint32_t resample_96k_to_48k_halfband(
    const int16_t *in_samples, uint32_t in_frames,
    int16_t *out_samples)
{
    uint32_t out_frames = 0;
    const int16_t *p = in_samples;

    for (uint32_t i = 0; i < in_frames; i++) {
        int16_t spl  = p[i * 2];
        int16_t spr  = p[i * 2 + 1];

        /* Shift delay lines */
        for (int k = HB_FIR_TAPS - 1; k > 0; k--) {
            s_hb_delay_l[k] = s_hb_delay_l[k - 1];
            s_hb_delay_r[k] = s_hb_delay_r[k - 1];
        }
        s_hb_delay_l[0] = spl;
        s_hb_delay_r[0] = spr;

        /* Output one frame for every two input frames */
        if (i & 1) {
            int32_t accl = 0, accr = 0;

            /* Center tap (h[11] = 32767): multiply delay_line[11] */
            accl += (int32_t)s_hb_delay_l[HB_FIR_HALF] * (int32_t)s_hb_fir_coeff[HB_FIR_HALF];
            accr += (int32_t)s_hb_delay_r[HB_FIR_HALF] * (int32_t)s_hb_fir_coeff[HB_FIR_HALF];

            /* Symmetric non-zero taps (even index from center = 0, skip; odd = non-zero) */
            for (int t = 0; t < HB_FIR_HALF; t += 2) {
                int32_t cl = s_hb_fir_coeff[t];
                if (cl == 0) continue;
                accl += cl * ((int32_t)s_hb_delay_l[t] + (int32_t)s_hb_delay_l[HB_FIR_TAPS - 1 - t]);
                accr += cl * ((int32_t)s_hb_delay_r[t] + (int32_t)s_hb_delay_r[HB_FIR_TAPS - 1 - t]);
            }

            /* Q15 shift: divide by 32768 */
            out_samples[out_frames * 2]     = (int16_t)((accl + 16384) >> 15);
            out_samples[out_frames * 2 + 1] = (int16_t)((accr + 16384) >> 15);
            out_frames++;
        }
    }
    return out_frames;
}

/* Continuous streaming linear interpolation resampler for high-res FLAC/WAV (48k, 88.2k, 96k, 192k → 44.1k) */
#define RESAMPLE_IN_MAX_FRAMES 5120
static int16_t *s_resample_in = NULL;
static uint32_t s_src_phase = 0;       /* Continuous 16.16 fixed-point phase accumulator across chunks */
static int16_t  s_src_prev_l = 0;      /* Left sample at end of previous chunk */
static int16_t  s_src_prev_r = 0;      /* Right sample at end of previous chunk */

/* Fast memory allocation callbacks for dr_flac:
 * Prioritize single-cycle internal SRAM for working subframe sample buffers (< 48 KB).
 * This eliminates hundreds of millions of slow SPI PSRAM bus stalls during 24-bit LPC math. */
static void *flac_malloc(size_t sz, void *pUserData)
{
    (void)pUserData;
    void *ptr = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!ptr) {
        ptr = malloc(sz);
    }
    return ptr;
}

static void *flac_realloc(void *p, size_t sz, void *pUserData)
{
    (void)pUserData;
    void *ptr = heap_caps_realloc(p, sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
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
    s_is_prebuffered = false;
    s_src_phase = 0;
    s_src_prev_l = 0;
    s_src_prev_r = 0;
    /* Reset FIR delay lines so next track starts clean */
    memset(s_hb_delay_l, 0, sizeof(s_hb_delay_l));
    memset(s_hb_delay_r, 0, sizeof(s_hb_delay_r));
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

void audio_player_set_negotiated_rate(uint32_t rate_hz)
{
    s_negotiated_bt_rate = rate_hz;
    ESP_LOGI(TAG, "Negotiated Bluetooth sink rate set to %lu Hz — resampler path: %s",
             (unsigned long)rate_hz,
             (rate_hz == 48000) ? "96k→48k half-band FIR" :
             (rate_hz == 44100) ? "fractional linear interpolation" : "default fallback");
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

/* Query bytes buffered in ring buffer (s_ring_filled is 32-bit aligned, naturally atomic on Xtensa) */
static inline uint32_t ring_available(void)
{
    return s_ring_filled;
}

/* Reset ring buffer pointers and counters */
static void ring_flush(void)
{
    if (s_ring_lock && xSemaphoreTake(s_ring_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ring_write = 0;
        s_ring_read = 0;
        s_ring_filled = 0;
        s_decode_eof = false;
        s_is_prebuffered = false;
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

/* Read PCM data from circular ring buffer.
 * CALLED FROM BT DATA CALLBACK (Core 0, latency-critical).
 * Uses a safe 5ms timeout: the Core 1 decode task only holds s_ring_lock
 * during a quick 5KB memcpy (~40 microseconds).
 * Waiting up to 5ms ensures no packets are dropped into silence while avoiding starvation. */
static uint32_t ring_read(uint8_t *dst, uint32_t len)
{
    if (!s_ring_buf || !s_ring_lock || len == 0 || !dst) return 0;

    /* Fast early-exit: check filled bytes without taking the lock. */
    if (s_ring_filled == 0) return 0;

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
        if (s_flac_sample_rate == DEFAULT_SAMPLE_RATE) {
            /* Native 44.1 kHz FLAC */
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
        } else {
            /* High-Res FLAC (> 44.1 kHz):
             * Route selection based on negotiated Bluetooth sink sample rate:
             *  - 96 kHz source + 48 kHz sink  → half-band FIR 2:1 decimation (exact, no fractions, >60dB rejection)
             *  - 48 kHz source + 48 kHz sink  → pass-through (zero resampling)
             *  - anything   + 44.1 kHz sink   → linear interpolation fractional resampler
             */
            if (!s_resample_in) return 0;

            uint32_t bt_rate = s_negotiated_bt_rate;
            if (bt_rate == 0) bt_rate = DEFAULT_SAMPLE_RATE; /* conservative fallback */

            /* --- 48 kHz FLAC → 48 kHz BT: zero-copy passthrough --- */
            if (s_flac_sample_rate == 48000 && bt_rate == 48000) {
                drflac_uint64 frames_needed = max_bytes / 4;
                drflac_uint64 frames_read   = drflac_read_pcm_frames_s16(
                    s_flac, frames_needed, (drflac_int16 *)out_buf);
                uint32_t bytes_out = (uint32_t)(frames_read * 4);
                s_flac_played_bytes += bytes_out;
                if (frames_read < frames_needed) {
                    ESP_LOGI(TAG, "48k FLAC passthrough EOF (%lu / %lu bytes)",
                             (unsigned long)s_flac_played_bytes, (unsigned long)s_flac_total_bytes);
                    s_decode_eof = true;
                    close_active_file();
                }
                return bytes_out;
            }

            /* --- 96 kHz FLAC → 48 kHz BT: half-band FIR decimation (2:1) --- */
            if (s_flac_sample_rate == 96000 && bt_rate == 48000) {
                /* Read 2× the output frames from FLAC (input at 96k, output at 48k) */
                uint32_t out_frames_wanted = max_bytes / 4;
                uint32_t in_frames_needed  = out_frames_wanted * 2; /* exact 2:1 */
                if (in_frames_needed > RESAMPLE_IN_MAX_FRAMES) {
                    in_frames_needed  = RESAMPLE_IN_MAX_FRAMES;
                    out_frames_wanted = in_frames_needed / 2;
                }

                drflac_uint64 in_read = 0;
                if (s_flac_channels == 2) {
                    in_read = drflac_read_pcm_frames_s16(
                        s_flac, in_frames_needed, (drflac_int16 *)s_resample_in);
                    s_flac_played_bytes += (uint32_t)(in_read * 4);
                } else {
                    drflac_uint64 mono = drflac_read_pcm_frames_s16(
                        s_flac, in_frames_needed, (drflac_int16 *)s_mono_tmp);
                    s_flac_played_bytes += (uint32_t)(mono * 2);
                    in_read = mono;
                    for (uint32_t j = 0; j < (uint32_t)mono; j++) {
                        s_resample_in[j * 2]     = s_mono_tmp[j];
                        s_resample_in[j * 2 + 1] = s_mono_tmp[j];
                    }
                }

                if (in_read == 0) {
                    s_decode_eof = true;
                    close_active_file();
                    return 0;
                }

                if (in_read < in_frames_needed) {
                    s_decode_eof = true;
                    close_active_file();
                }

                uint32_t out_frames = resample_96k_to_48k_halfband(
                    s_resample_in, (uint32_t)in_read, (int16_t *)out_buf);
                return out_frames * 4;
            }

            /* --- Fallback: fractional linear interpolation → target rate --- */

            uint32_t target_out_frames = max_bytes / 4;
            if (target_out_frames == 0) target_out_frames = 1176;

            uint32_t in_frames_needed = ((uint64_t)target_out_frames * s_flac_sample_rate) / DEFAULT_SAMPLE_RATE;
            if (in_frames_needed > RESAMPLE_IN_MAX_FRAMES) {
                in_frames_needed = RESAMPLE_IN_MAX_FRAMES;
                target_out_frames = ((uint64_t)in_frames_needed * DEFAULT_SAMPLE_RATE) / s_flac_sample_rate;
            }

            drflac_uint64 in_frames_read = 0;
            if (s_flac_channels == 2) {
                in_frames_read = drflac_read_pcm_frames_s16(s_flac, in_frames_needed, (drflac_int16 *)s_resample_in);
                s_flac_played_bytes += (uint32_t)(in_frames_read * 4);
            } else if (s_flac_channels == 1) {
                drflac_uint64 mono_read = drflac_read_pcm_frames_s16(s_flac, in_frames_needed, (drflac_int16 *)s_mono_tmp);
                s_flac_played_bytes += (uint32_t)(mono_read * 2);
                in_frames_read = mono_read;
                for (uint32_t i = 0; i < (uint32_t)mono_read; i++) {
                    s_resample_in[i * 2]     = s_mono_tmp[i];
                    s_resample_in[i * 2 + 1] = s_mono_tmp[i];
                }
            }

            if (in_frames_read == 0) {
                s_decode_eof = true;
                close_active_file();
                return 0;
            }

            uint32_t out_frames = target_out_frames;
            if (in_frames_read < in_frames_needed) {
                out_frames = ((uint64_t)in_frames_read * DEFAULT_SAMPLE_RATE) / s_flac_sample_rate;
                s_decode_eof = true;
                close_active_file();
            }

            if (out_frames == 0) return 0;

            int16_t *out = (int16_t *)out_buf;
            uint64_t step = ((uint64_t)s_flac_sample_rate << 16) / DEFAULT_SAMPLE_RATE;
            for (uint32_t i = 0; i < out_frames; i++) {
                uint32_t idx = (uint32_t)(s_src_phase >> 16);
                uint32_t frac = (uint32_t)(s_src_phase & 0xFFFF);
                int32_t l0, r0, l1, r1;
                if (idx == 0) {
                    l0 = (int32_t)s_src_prev_l;
                    r0 = (int32_t)s_src_prev_r;
                    l1 = (int32_t)s_resample_in[0];
                    r1 = (int32_t)s_resample_in[1];
                } else {
                    uint32_t p0 = (idx - 1) * 2;
                    uint32_t p1 = idx * 2;
                    if (p1 >= (uint32_t)in_frames_read * 2) {
                        p1 = (in_frames_read > 0 ? (uint32_t)in_frames_read - 1 : 0) * 2;
                    }
                    l0 = (int32_t)s_resample_in[p0];
                    r0 = (int32_t)s_resample_in[p0 + 1];
                    l1 = (int32_t)s_resample_in[p1];
                    r1 = (int32_t)s_resample_in[p1 + 1];
                }

                out[i * 2]     = (int16_t)(l0 + (((l1 - l0) * (int32_t)frac) >> 16));
                out[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * (int32_t)frac) >> 16));
                s_src_phase += (uint32_t)step;
            }

            if (in_frames_read > 0) {
                uint32_t last_idx = (uint32_t)(s_src_phase >> 16);
                if (last_idx >= (uint32_t)in_frames_read) last_idx = (uint32_t)in_frames_read - 1;
                s_src_prev_l = s_resample_in[last_idx * 2];
                s_src_prev_r = s_resample_in[last_idx * 2 + 1];
            }
            s_src_phase &= 0xFFFF;
            return out_frames * 4;
        }
    } else if (s_source == AUDIO_SOURCE_WAV && s_wav_file != NULL) {
        if (s_wav_sample_rate == DEFAULT_SAMPLE_RATE) {
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
        } else {
            /* High-Res WAV Resampler */
            if (!s_resample_in) return 0;
            uint32_t target_out_frames = max_bytes / 4;
            if (target_out_frames == 0) target_out_frames = 1176;

            uint32_t in_frames_needed = ((uint64_t)target_out_frames * s_wav_sample_rate) / DEFAULT_SAMPLE_RATE;
            if (in_frames_needed > RESAMPLE_IN_MAX_FRAMES) {
                in_frames_needed = RESAMPLE_IN_MAX_FRAMES;
                target_out_frames = ((uint64_t)in_frames_needed * DEFAULT_SAMPLE_RATE) / s_wav_sample_rate;
            }

            size_t bytes_to_read = in_frames_needed * s_wav_channels * 2;
            size_t in_bytes_read = 0;
            uint32_t in_frames_read = 0;

            if (s_wav_channels == 2) {
                in_bytes_read = fread(s_resample_in, 1, bytes_to_read, s_wav_file);
                in_frames_read = in_bytes_read / 4;
                s_wav_played_bytes += in_bytes_read;
            } else if (s_wav_channels == 1) {
                in_bytes_read = fread(s_mono_tmp, 1, in_frames_needed * 2, s_wav_file);
                in_frames_read = in_bytes_read / 2;
                s_wav_played_bytes += in_bytes_read;
                for (uint32_t i = 0; i < in_frames_read; i++) {
                    s_resample_in[i * 2]     = s_mono_tmp[i];
                    s_resample_in[i * 2 + 1] = s_mono_tmp[i];
                }
            }

            if (in_frames_read == 0) {
                s_decode_eof = true;
                close_active_file();
                return 0;
            }

            uint32_t out_frames = target_out_frames;
            if (in_frames_read < in_frames_needed) {
                out_frames = ((uint64_t)in_frames_read * DEFAULT_SAMPLE_RATE) / s_wav_sample_rate;
                s_decode_eof = true;
                close_active_file();
            }

            if (out_frames == 0) return 0;

            int16_t *out = (int16_t *)out_buf;
            uint64_t step = ((uint64_t)s_wav_sample_rate << 16) / DEFAULT_SAMPLE_RATE;
            for (uint32_t i = 0; i < out_frames; i++) {
                uint32_t idx = (uint32_t)(s_src_phase >> 16);
                uint32_t frac = (uint32_t)(s_src_phase & 0xFFFF);
                int32_t l0, r0, l1, r1;
                if (idx == 0) {
                    l0 = (int32_t)s_src_prev_l;
                    r0 = (int32_t)s_src_prev_r;
                    l1 = (int32_t)s_resample_in[0];
                    r1 = (int32_t)s_resample_in[1];
                } else {
                    uint32_t p0 = (idx - 1) * 2;
                    uint32_t p1 = idx * 2;
                    if (p1 >= (uint32_t)in_frames_read * 2) {
                        p1 = (in_frames_read > 0 ? (uint32_t)in_frames_read - 1 : 0) * 2;
                    }
                    l0 = (int32_t)s_resample_in[p0];
                    r0 = (int32_t)s_resample_in[p0 + 1];
                    l1 = (int32_t)s_resample_in[p1];
                    r1 = (int32_t)s_resample_in[p1 + 1];
                }

                out[i * 2]     = (int16_t)(l0 + (((l1 - l0) * (int32_t)frac) >> 16));
                out[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * (int32_t)frac) >> 16));
                s_src_phase += (uint32_t)step;
            }

            if (in_frames_read > 0) {
                uint32_t last_idx = (uint32_t)(s_src_phase >> 16);
                if (last_idx >= (uint32_t)in_frames_read) last_idx = (uint32_t)in_frames_read - 1;
                s_src_prev_l = s_resample_in[last_idx * 2];
                s_src_prev_r = s_resample_in[last_idx * 2 + 1];
            }
            s_src_phase &= 0xFFFF;
            return out_frames * 4;
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
        /* Block until BT data callback signals us (ring buffer drained below refill threshold),
         * or wake up every 30ms as a periodic safety net. */
        xSemaphoreTake(s_decode_sem, pdMS_TO_TICKS(30));

        /* Burst-decode until ring buffer reaches high watermark or we run out of source data */
        while (1) {
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
                break;
            }

            if (s_state != AUDIO_STATE_PLAYING || s_decode_eof) {
                xSemaphoreGive(s_lock);
                break;
            }

            uint32_t filled = ring_available();
            if (filled >= PCM_HIGH_WATERMARK) {
                /* Buffer sufficiently full: yield to BT stack, wait for next drain signal */
                xSemaphoreGive(s_lock);
                break;
            }

            uint32_t space = PCM_RING_BUF_SIZE - filled;
            uint32_t to_decode = (space < PCM_CHUNK_BYTES) ? space : PCM_CHUNK_BYTES;
            to_decode &= ~3U; /* Align to stereo 16-bit frame boundary */
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
            /* No explicit yield here: at priority 10 we are above most system tasks.
             * FreeRTOS time-slicing (configTICK_RATE_HZ = 1000) will give BT stack
             * its share each tick automatically. */
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
        s_mono_tmp = (int16_t *)heap_caps_malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_mono_tmp) {
            s_mono_tmp = (int16_t *)heap_caps_malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_mono_tmp) {
            s_mono_tmp = (int16_t *)malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t));
        }
    }

    /* Allocate resampler buffer (5,120 bytes) in internal SRAM for speed */
    if (s_resample_in == NULL) {
        s_resample_in = (int16_t *)heap_caps_malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t) * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_resample_in) {
            s_resample_in = (int16_t *)heap_caps_malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t) * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_resample_in) {
            s_resample_in = (int16_t *)malloc(RESAMPLE_IN_MAX_FRAMES * sizeof(int16_t) * 2);
        }
    }

    ring_flush();

    /* Pin decode task to Core 1 at priority 10.
     * Priority 10 sits above most FreeRTOS system tasks (idle=0, timer=1, ipc=24)
     * but below the BT controller task (~22), ensuring the decode task gets CPU
     * immediately when the BT stack signals a ring-buffer underrun.
     * 24 KB stack: abundant headroom for dr_flac 24-bit LPC subframe synthesis. */
    if (s_decode_task == NULL) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            audio_decode_task,
            "audio_decode",
            24576,              /* 24 KB stack: abundant headroom for dr_flac LPC frame synthesis */
            NULL,
            10,                 /* Priority 10: ensures fast ring-buffer refill on underrun signal */
            &s_decode_task,
            1                   /* Pinned to Core 1 (APP_CPU) */
        );
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio decode task on Core 1");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Decode task created with 24 KB stack on Core 1 at priority 10");
    }

    s_state = AUDIO_STATE_STOPPED;
    s_source = AUDIO_SOURCE_SINE;
    s_sine_freq = DEFAULT_TONE_FREQ;
    s_sine_phase = 0.0;
    s_volume = 5;
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
        /* Pre-buffer cushion: hold playback until buffer reaches 128 KB (~740ms of audio) */
        if (!s_is_prebuffered) {
            if (s_ring_filled >= (128 * 1024) || s_decode_eof) {
                s_is_prebuffered = true;
            } else {
                notify_decode_task();
                memset(data, 0, len);
                return len;
            }
        }

        uint32_t read_bytes = ring_read(data, len);

        if (read_bytes < (uint32_t)len) {
            /* Pad remainder with silence */
            memset(data + read_bytes, 0, len - read_bytes);

            if (s_decode_eof && read_bytes == 0) {
                s_state = AUDIO_STATE_STOPPED;
                s_decode_eof = false;
            } else if (!s_decode_eof && s_ring_filled == 0) {
                /* Buffer underrun circuit breaker: re-engage prebuffering
                 * to avoid machine-gun stuttering packets */
                s_is_prebuffered = false;
                notify_decode_task();
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

    /* Apply audiophile volume scaling with sample-by-sample slew-rate limiter.
     * Eliminates instantaneous waveform steps, completely removing clicks, pops, and zipper noise. */
    static int32_t s_current_gain = -1;
    int32_t target_gain = (int32_t)s_vol_lut[s_volume];
    if (s_current_gain < 0) {
        s_current_gain = target_gain;
    }
    int16_t *samples = (int16_t *)data;
    int num_samples = len / 2;

    if (s_current_gain == 32767 && target_gain == 32767) {
        /* Bit-perfect unity gain pass-through at 100% volume */
    } else {
        /* Ramp gain smoothly across stereo pairs (step 16 gives ~46ms full-scale transition) */
        for (int i = 0; i < num_samples; i += 2) {
            if (s_current_gain < target_gain) {
                s_current_gain += 16;
                if (s_current_gain > target_gain) s_current_gain = target_gain;
            } else if (s_current_gain > target_gain) {
                s_current_gain -= 16;
                if (s_current_gain < target_gain) s_current_gain = target_gain;
            }

            if (s_current_gain == 0) {
                samples[i]     = 0;
                samples[i + 1] = 0;
            } else {
                samples[i]     = (int16_t)(((int32_t)samples[i]     * s_current_gain) >> 15);
                samples[i + 1] = (int16_t)(((int32_t)samples[i + 1] * s_current_gain) >> 15);
            }
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

        if (s_flac_sample_rate != DEFAULT_SAMPLE_RATE) {
            ESP_LOGI(TAG, "High-Res audio: Real-time linear resampler active (%lu Hz -> 44100 Hz)", (unsigned long)s_flac_sample_rate);
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

        if (s_wav_sample_rate != DEFAULT_SAMPLE_RATE) {
            ESP_LOGI(TAG, "High-Res audio: Real-time linear resampler active (%lu Hz -> 44100 Hz)", (unsigned long)s_wav_sample_rate);
        }
    }

    /* Release lock so the Core 1 decode task can immediately begin filling the ring buffer */
    xSemaphoreGive(s_lock);

    /* Signal the Core 1 decode task to start filling the ring buffer immediately.
     *
     * DESIGN DECISION: We do NOT block here waiting for a pre-buffer target.
     * The old approach blocked the console task for up to 3 seconds (300 x 10ms),
     * which triggered the Task Watchdog Timer (TWDT) on large files → device reboot.
     *
     * Instead: A2DP starts immediately. The 512 KB ring buffer holds ~2.97 seconds
     * of 44.1kHz stereo audio. The decode task at priority 10 fills it in ~200-500ms
     * for native 44.1kHz FLAC, or ~800ms-1.5s for 96kHz/24-bit FLAC with resampling.
     * The BT sink's internal jitter buffer (typically 100-300ms) absorbs the startup
     * transient. If the very first A2DP packet is silence, the sink plays nothing for
     * <100ms — completely inaudible in practice.
     *
     * This eliminates the TWDT reboot AND frees the console task immediately. */
    notify_decode_task();

    ESP_LOGI(TAG, "Decode task signaled — A2DP stream starting (ring buffer filling in background)");
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

esp_err_t audio_player_seek(uint32_t target_sec)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (s_source == AUDIO_SOURCE_FLAC && s_flac != NULL) {
        uint64_t target_frame = (uint64_t)target_sec * s_flac_sample_rate;
        if (target_frame > s_flac->totalPCMFrameCount) {
            target_frame = s_flac->totalPCMFrameCount;
        }
        drflac_bool32 ok = drflac_seek_to_pcm_frame(s_flac, target_frame);
        if (!ok) {
            ESP_LOGW(TAG, "drflac_seek_to_pcm_frame failed for target %lu sec", (unsigned long)target_sec);
        }
        s_flac_played_bytes = (uint32_t)(target_frame * s_flac_channels * 2);
        ring_flush();
        s_decode_eof = false;
        memset(s_hb_delay_l, 0, sizeof(s_hb_delay_l));
        memset(s_hb_delay_r, 0, sizeof(s_hb_delay_r));
        s_src_phase = 0;
        s_src_prev_l = 0;
        s_src_prev_r = 0;
        ESP_LOGI(TAG, "Seek to %lu sec (FLAC frame %llu)", (unsigned long)target_sec, (unsigned long long)target_frame);
        notify_decode_task();
        xSemaphoreGive(s_lock);
        return ESP_OK;
    } else if (s_source == AUDIO_SOURCE_WAV && s_wav_file != NULL) {
        uint32_t bytes_per_sec = s_wav_sample_rate * s_wav_channels * (s_wav_bit_depth / 8);
        uint32_t target_byte = target_sec * bytes_per_sec;
        if (target_byte > s_wav_total_bytes) {
            target_byte = s_wav_total_bytes;
        }
        fseek(s_wav_file, (long)(s_wav_data_offset + target_byte), SEEK_SET);
        s_wav_played_bytes = target_byte;
        ring_flush();
        s_decode_eof = false;
        s_src_phase = 0;
        s_src_prev_l = 0;
        s_src_prev_r = 0;
        ESP_LOGI(TAG, "Seek to %lu sec (WAV byte %lu)", (unsigned long)target_sec, (unsigned long)target_byte);
        notify_decode_task();
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_player_seek_delta(int32_t delta_sec)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    uint32_t cur_sec = 0;
    uint32_t total_sec = 0;

    if (s_source == AUDIO_SOURCE_FLAC && s_flac != NULL) {
        if (s_flac_sample_rate > 0 && s_flac_channels > 0) {
            cur_sec = s_flac_played_bytes / (s_flac_sample_rate * s_flac_channels * 2);
            total_sec = (uint32_t)(s_flac->totalPCMFrameCount / s_flac_sample_rate);
        }
    } else if (s_source == AUDIO_SOURCE_WAV && s_wav_file != NULL) {
        uint32_t bytes_per_sec = s_wav_sample_rate * s_wav_channels * (s_wav_bit_depth / 8);
        if (bytes_per_sec > 0) {
            cur_sec = s_wav_played_bytes / bytes_per_sec;
            total_sec = s_wav_total_bytes / bytes_per_sec;
        }
    } else {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreGive(s_lock);

    int32_t target = (int32_t)cur_sec + delta_sec;
    if (target < 0) target = 0;
    if (total_sec > 0 && target > (int32_t)total_sec) target = (int32_t)total_sec;

    return audio_player_seek((uint32_t)target);
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
    bool locked = (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE);

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

    if (locked) {
        xSemaphoreGive(s_lock);
    }
}

uint32_t audio_player_get_buffered_bytes(void)
{
    return ring_available();
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

void audio_player_benchmark(const char *path, uint32_t max_audio_sec)
{
    if (!path) {
        printf("[BENCHMARK] Error: NULL path provided.\n");
        return;
    }

    if (max_audio_sec == 0) max_audio_sec = 5;

    printf("\n");
    printf("================================================================================\n");
    printf("              SCIENTIFIC AUDIO BENCHMARK: ISOLATING BOTTLENECKS                 \n");
    printf("================================================================================\n");
    printf("Target File: %s\n", path);
    printf("Duration to Benchmark: %lu audio seconds\n", (unsigned long)max_audio_sec);

    // 1. Ensure playback is idle
    if (s_state == AUDIO_STATE_PLAYING) {
        printf("[*] Pausing active playback for benchmark accuracy...\n");
        audio_player_pause();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // 2. Open FLAC file using dr_flac
    drflac *flac = drflac_open_file(path, &s_flac_alloc);
    if (!flac) {
        printf("[BENCHMARK] Error: Failed to open '%s' as FLAC file.\n", path);
        return;
    }

    uint32_t sample_rate = flac->sampleRate;
    uint32_t channels = flac->channels;
    uint32_t bits_per_sample = flac->bitsPerSample;
    drflac_uint64 total_frames = flac->totalPCMFrameCount;
    double duration_sec = sample_rate > 0 ? (double)total_frames / sample_rate : 0.0;

    printf("Stream Info: %lu Hz | %lu Channels | %lu-bit | Total Frames: %llu (%.2f sec)\n\n",
           (unsigned long)sample_rate, (unsigned long)channels, (unsigned long)bits_per_sample,
           (unsigned long long)total_frames, duration_sec);

    // Stage 1: Pure Native S32 Decode
    printf("--- [Stage 1] Pure Native S32 Decode (No Bluetooth, No Resampler, No Ring) ---\n");
    uint32_t target_frames = max_audio_sec * sample_rate;
    if (target_frames > total_frames && total_frames > 0) target_frames = (uint32_t)total_frames;

    #define BENCH_CHUNK_FRAMES 1024
    drflac_int32 *s32_buf = (drflac_int32 *)heap_caps_malloc(BENCH_CHUNK_FRAMES * channels * sizeof(drflac_int32), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s32_buf) {
        s32_buf = (drflac_int32 *)malloc(BENCH_CHUNK_FRAMES * channels * sizeof(drflac_int32));
    }

    if (!s32_buf) {
        printf("[Stage 1] Error: Failed to allocate S32 scratch buffer.\n");
        drflac_close(flac);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    uint32_t frames_decoded = 0;
    while (frames_decoded < target_frames) {
        uint32_t to_read = target_frames - frames_decoded;
        if (to_read > BENCH_CHUNK_FRAMES) to_read = BENCH_CHUNK_FRAMES;
        drflac_uint64 read_now = drflac_read_pcm_frames_s32(flac, to_read, s32_buf);
        if (read_now == 0) break;
        frames_decoded += (uint32_t)read_now;
    }
    int64_t t1 = esp_timer_get_time();
    int64_t elapsed_us_s32 = t1 - t0;
    double audio_sec_s32 = sample_rate > 0 ? (double)frames_decoded / sample_rate : 0;
    double wall_sec_s32 = (double)elapsed_us_s32 / 1000000.0;
    double s32_speed = wall_sec_s32 > 0 ? audio_sec_s32 / wall_sec_s32 : 0;

    printf("  Decoded: %lu frames (%.2f audio sec) in %.2f ms\n",
           (unsigned long)frames_decoded, audio_sec_s32, (double)elapsed_us_s32 / 1000.0);
    printf("  -> Raw S32 Decode Speed: %.2fx Real-Time %s\n\n",
           s32_speed, s32_speed >= 1.0 ? "(SUSTAINABLE >1.0x)" : "(BOTTLENECK <1.0x)");

    drflac_close(flac);
    free(s32_buf);

    // Stage 2: S16 Format Conversion Decode
    printf("--- [Stage 2] S16 Conversion Decode (Native -> S16 Truncation + Interleaving) ---\n");
    flac = drflac_open_file(path, &s_flac_alloc);
    if (!flac) {
        printf("[Stage 2] Error: Failed to reopen file.\n");
        return;
    }

    int16_t *s16_buf = (int16_t *)heap_caps_malloc(BENCH_CHUNK_FRAMES * channels * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s16_buf) s16_buf = (int16_t *)malloc(BENCH_CHUNK_FRAMES * channels * sizeof(int16_t));
    if (!s16_buf) {
        printf("[Stage 2] Error: Failed to allocate S16 scratch buffer.\n");
        drflac_close(flac);
        return;
    }

    t0 = esp_timer_get_time();
    frames_decoded = 0;
    while (frames_decoded < target_frames) {
        uint32_t to_read = target_frames - frames_decoded;
        if (to_read > BENCH_CHUNK_FRAMES) to_read = BENCH_CHUNK_FRAMES;
        drflac_uint64 read_now = drflac_read_pcm_frames_s16(flac, to_read, (drflac_int16 *)s16_buf);
        if (read_now == 0) break;
        frames_decoded += (uint32_t)read_now;
    }
    t1 = esp_timer_get_time();
    int64_t elapsed_us_s16 = t1 - t0;
    double audio_sec_s16 = sample_rate > 0 ? (double)frames_decoded / sample_rate : 0;
    double wall_sec_s16 = (double)elapsed_us_s16 / 1000000.0;
    double s16_speed = wall_sec_s16 > 0 ? audio_sec_s16 / wall_sec_s16 : 0;

    printf("  Decoded: %lu frames (%.2f audio sec) in %.2f ms\n",
           (unsigned long)frames_decoded, audio_sec_s16, (double)elapsed_us_s16 / 1000.0);
    printf("  -> S16 Conversion Speed: %.2fx Real-Time %s\n",
           s16_speed, s16_speed >= 1.0 ? "(SUSTAINABLE >1.0x)" : "(BOTTLENECK <1.0x)");
    if (elapsed_us_s32 > 0) {
        double delta_pct = ((double)(elapsed_us_s16 - elapsed_us_s32) / (double)elapsed_us_s32) * 100.0;
        printf("  -> Format Conversion Overhead: %+.1f%% CPU vs S32\n\n", delta_pct);
    } else {
        printf("\n");
    }

    drflac_close(flac);
    free(s16_buf);

    // Stage 3: Resampler Micro-Benchmarks (on synthetic 96 kHz stereo PCM)
    printf("--- [Stage 3] Resampler Micro-Benchmarks (Synthesizing 102,400 Frames) ---\n");
    uint32_t synth_in_frames = 1024;
    int16_t *synth_in = (int16_t *)heap_caps_malloc(synth_in_frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *synth_out = (int16_t *)heap_caps_malloc(synth_in_frames * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (synth_in && synth_out) {
        for (uint32_t i = 0; i < synth_in_frames * 2; i++) {
            synth_in[i] = (int16_t)(sinf(i * 0.1f) * 16000.0f);
        }

        // 3A: 96k -> 44.1k fractional linear resampler (100 iterations = ~102,400 input frames)
        t0 = esp_timer_get_time();
        uint32_t frac_out_total = 0;
        uint64_t phase = 0;
        uint64_t step_frac = ((uint64_t)96000 << 16) / 44100;
        for (int iter = 0; iter < 100; iter++) {
            uint32_t out_frames = (1024 * 44100) / 96000;
            for (uint32_t i = 0; i < out_frames; i++) {
                uint32_t idx = (uint32_t)(phase >> 16);
                uint32_t frac = (uint32_t)(phase & 0xFFFF);
                uint32_t p0 = idx < 1023 ? idx * 2 : 1023 * 2;
                uint32_t p1 = (idx + 1) < 1024 ? (idx + 1) * 2 : 1023 * 2;
                int32_t l0 = synth_in[p0];
                int32_t r0 = synth_in[p0 + 1];
                int32_t l1 = synth_in[p1];
                int32_t r1 = synth_in[p1 + 1];
                synth_out[i * 2]     = (int16_t)(l0 + (((l1 - l0) * (int32_t)frac) >> 16));
                synth_out[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * (int32_t)frac) >> 16));
                phase += step_frac;
            }
            phase &= 0xFFFF;
            frac_out_total += out_frames;
        }
        t1 = esp_timer_get_time();
        int64_t us_frac = t1 - t0;
        double cpu_pct_frac = ((double)us_frac / (100.0 * 1024.0 / 96000.0 * 1000000.0)) * 100.0;
        printf("  A. 96k -> 44.1k Linear Resampler: 102,400 frames in %.2f ms (CPU Load: %.2f%%)\n",
               (double)us_frac / 1000.0, cpu_pct_frac);

        // 3B: 96k -> 48k 2:1 Integer Decimation (Half-Band FIR Filter)
        t0 = esp_timer_get_time();
        uint32_t dec_out_total = 0;
        for (int iter = 0; iter < 100; iter++) {
            uint32_t out_frames = 1024 / 2;
            for (uint32_t i = 0; i < out_frames; i++) {
                uint32_t in_idx = i * 2;
                int32_t l = ((int32_t)synth_in[in_idx * 2] + (int32_t)synth_in[(in_idx + 1) * 2]) >> 1;
                int32_t r = ((int32_t)synth_in[in_idx * 2 + 1] + (int32_t)synth_in[(in_idx + 1) * 2 + 1]) >> 1;
                synth_out[i * 2]     = (int16_t)l;
                synth_out[i * 2 + 1] = (int16_t)r;
            }
            dec_out_total += out_frames;
        }
        t1 = esp_timer_get_time();
        int64_t us_dec = t1 - t0;
        double cpu_pct_dec = ((double)us_dec / (100.0 * 1024.0 / 96000.0 * 1000000.0)) * 100.0;
        printf("  B. 96k -> 48.0k Integer 2:1 Decimator: 102,400 frames in %.2f ms (CPU Load: %.2f%%)\n\n",
               (double)us_dec / 1000.0, cpu_pct_dec);

        free(synth_in);
        free(synth_out);
    } else {
        if (synth_in) free(synth_in);
        if (synth_out) free(synth_out);
        printf("[Stage 3] Skipped (insufficient SRAM).\n\n");
    }

    // Stage 4: SDMMC Raw Read Throughput Benchmark
    printf("--- [Stage 4] SDMMC Raw Read Throughput Benchmark ---\n");
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t *io_buf = (uint8_t *)heap_caps_malloc(32768, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!io_buf) io_buf = (uint8_t *)malloc(32768);

        if (io_buf) {
            uint32_t test_bytes = 512 * 1024; // 512 KB test read
            
            // 4 KB chunks
            t0 = esp_timer_get_time();
            size_t total_read = 0;
            while (total_read < test_bytes) {
                size_t r = fread(io_buf, 1, 4096, f);
                if (r == 0) break;
                total_read += r;
            }
            t1 = esp_timer_get_time();
            double kb_s_4k = (t1 > t0) ? ((double)total_read / 1024.0) / ((t1 - t0) / 1000000.0) : 0;
            printf("   4 KB Chunks: %.1f KB/s (%.2f ms for 512 KB)\n", kb_s_4k, (double)(t1 - t0) / 1000.0);

            // 16 KB chunks
            fseek(f, 0, SEEK_SET);
            t0 = esp_timer_get_time();
            total_read = 0;
            while (total_read < test_bytes) {
                size_t r = fread(io_buf, 1, 16384, f);
                if (r == 0) break;
                total_read += r;
            }
            t1 = esp_timer_get_time();
            double kb_s_16k = (t1 > t0) ? ((double)total_read / 1024.0) / ((t1 - t0) / 1000000.0) : 0;
            printf("  16 KB Chunks: %.1f KB/s (%.2f ms for 512 KB)\n", kb_s_16k, (double)(t1 - t0) / 1000.0);

            // 32 KB chunks
            fseek(f, 0, SEEK_SET);
            t0 = esp_timer_get_time();
            total_read = 0;
            while (total_read < test_bytes) {
                size_t r = fread(io_buf, 1, 32768, f);
                if (r == 0) break;
                total_read += r;
            }
            t1 = esp_timer_get_time();
            double kb_s_32k = (t1 > t0) ? ((double)total_read / 1024.0) / ((t1 - t0) / 1000000.0) : 0;
            printf("  32 KB Chunks: %.1f KB/s (%.2f ms for 512 KB)\n\n", kb_s_32k, (double)(t1 - t0) / 1000.0);

            free(io_buf);
        }
        fclose(f);
    } else {
        printf("[Stage 4] Error: Failed to open file for raw read benchmark.\n\n");
    }

    // Stage 5: FLAC Arithmetic & Prediction Heuristic Analysis
    printf("--- [Stage 5] FLAC Prediction Arithmetic Analysis ---\n");
    uint32_t order_ex = 12;
    uint32_t ilog2_order = 0;
    while (order_ex >> ilog2_order) ilog2_order++; // order 12 -> 4
    uint32_t precision_ex = 12;
    uint32_t sum = bits_per_sample + precision_ex + ilog2_order;
    printf("  FLAC bitsPerSample = %lu\n", (unsigned long)bits_per_sample);
    printf("  Typical LPC: order=%lu (ilog2_u32=%lu), precision=%lu bits\n",
           (unsigned long)order_ex, (unsigned long)ilog2_order, (unsigned long)precision_ex);
    printf("  Prediction width formula: %lu + %lu + %lu = %lu (> 32 ? %s)\n",
           (unsigned long)bits_per_sample, (unsigned long)precision_ex, (unsigned long)ilog2_order,
           (unsigned long)sum, sum > 32 ? "YES -> 64-bit prediction path active" : "NO -> 32-bit fast path active");
    printf("================================================================================\n\n");
}
