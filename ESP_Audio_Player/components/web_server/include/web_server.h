#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the embedded HTTP web server on port 80.
 *
 * Registers endpoints:
 *   - GET  /                  : Main Dashboard UI
 *   - GET  /api/status        : Real-time system & playback JSON status
 *   - GET  /api/files         : MicroSD card directory listing JSON
 *   - POST /api/player/play   : Resume playback
 *   - POST /api/player/pause  : Pause playback
 *   - POST /api/player/stop   : Stop playback
 *   - POST /api/player/play_file : Play specific audio file
 *   - POST /api/player/tone   : Set test sine tone
 *   - POST /api/system/restart: Reboot ESP32
 *
 * @return ESP_OK on success
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the embedded HTTP web server.
 *
 * @return ESP_OK on success
 */
esp_err_t web_server_stop(void);

/**
 * @brief Check if web server is currently running.
 *
 * @return true if running, false otherwise
 */
bool web_server_is_running(void);

#ifdef __cplusplus
}
#endif
