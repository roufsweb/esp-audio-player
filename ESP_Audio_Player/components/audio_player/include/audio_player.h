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
    AUDIO_SOURCE_WAV
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
 * @brief Pause active playback.
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
