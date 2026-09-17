#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_STATE_STOPPED,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} audio_player_state_t;

typedef enum {
    AUDIO_SOURCE_NONE,
    AUDIO_SOURCE_SINE,
    AUDIO_SOURCE_WAV,
    AUDIO_SOURCE_FLAC
} audio_player_source_t;

typedef struct {
    audio_player_state_t state;
    audio_player_source_t source;
    char current_path[256];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bit_depth;
    uint32_t total_audio_bytes;
    uint32_t played_audio_bytes;
} audio_player_status_t;

typedef enum {
    AUDIO_PLAYER_CMD_START = 0,
    AUDIO_PLAYER_CMD_SUSPEND,
    AUDIO_PLAYER_CMD_STOP
} audio_player_cmd_t;

typedef void (*audio_player_media_ctrl_cb_t)(audio_player_cmd_t cmd);

/**
 * @brief Register optional media control callback (e.g. A2DP stream start/stop).
 */
void audio_player_set_media_ctrl_cb(audio_player_media_ctrl_cb_t cb);

/**
 * @brief Set digital volume percentage (0 to 100).
 */
void audio_player_set_volume(uint8_t volume_pct);

/**
 * @brief Get current digital volume percentage.
 */
uint8_t audio_player_get_volume(void);

/**
 * @brief Notify the audio player of the Bluetooth sink's negotiated sample rate.
 *        Call this from the A2DP codec-configured callback after AVDTP handshake.
 *        The player uses this to select the optimal resampler path:
 *        - 48000: use half-band FIR for 96k→48k (exact 2:1, >60dB rejection)
 *        - 44100: use fractional linear interpolation resampler
 *        - 0: unknown, fall back to 44.1 kHz path
 *
 * @param rate_hz Negotiated sample rate in Hz (44100, 48000, or 0 for unknown)
 */
void audio_player_set_negotiated_rate(uint32_t rate_hz);

/**
 * @brief Initialize the audio player engine.
 */
esp_err_t audio_player_init(void);

/**
 * @brief Audio data feeder callback passed to A2DP source.
 *        Pulls decoded PCM data (16-bit 44.1 kHz stereo) into the A2DP stream.
 */
int32_t audio_player_data_cb(uint8_t *data, int32_t len);

/**
 * @brief Open and begin playback of an uncompressed WAV file from SD card.
 *
 * @param path Full path to WAV file (e.g. "/sdcard/music/test.wav")
 * @return ESP_OK on success, or error code on invalid format / file missing
 */
esp_err_t audio_player_play_file(const char *path);

/**
 * @brief Switch audio source to mathematical sine test tone.
 *
 * @param freq_hz Frequency in Hz (e.g. 440.0, 1000.0)
 */
esp_err_t audio_player_set_tone(double freq_hz);

/**
 * @brief Resume active playback.
 */
esp_err_t audio_player_play(void);

/**
 * @brief Seek to an absolute position in the active audio file.
 *
 * @param target_sec Target position in seconds from start of track
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not playing, or ESP_ERR_NOT_SUPPORTED
 */
esp_err_t audio_player_seek(uint32_t target_sec);

/**
 * @brief Seek forward or backward by a delta relative to current position.
 *
 * @param delta_sec Signed delta in seconds (e.g. +10 for fast-forward, -10 for rewind)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not playing
 */
esp_err_t audio_player_seek_delta(int32_t delta_sec);

/**
 * @brief Pause audio playback.
 */
esp_err_t audio_player_pause(void);

/**
 * @brief Stop active playback and close file if open.
 */
esp_err_t audio_player_stop(void);

/**
 * @brief Get current playback status snapshot.
 */
void audio_player_get_status(audio_player_status_t *out_status);

/**
 * @brief Print formatted playback status to stdout.
 */
void audio_player_print_status(void);

/**
 * @brief Get current number of audio bytes buffered in the PSRAM ring buffer.
 */
uint32_t audio_player_get_buffered_bytes(void);


/**
 * @brief Run a scientific micro-benchmark on an audio file isolating SDMMC, FLAC S32/S16 decode, and resamplers.
 *
 * @param path Full path to audio file on SD card
 * @param max_audio_sec Number of audio seconds to test (default: 5)
 */
void audio_player_benchmark(const char *path, uint32_t max_audio_sec);

/**
 * @brief Register audio player console commands:
 *        - play_file <path>
 *        - play
 *        - pause
 *        - stop
 *        - tone [freq]
 *        - status
 */
void audio_player_register_console_commands(void);

#ifdef __cplusplus
}
#endif
