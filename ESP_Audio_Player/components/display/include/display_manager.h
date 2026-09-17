#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware Pinout for ESP32-CAM HW-297 (Reclaimed Camera & Freed SDMMC DAT3) */
#define DISPLAY_PIN_CS    5     /* Camera Y2 */
#define DISPLAY_PIN_SCK   18    /* Camera Y3 */
#define DISPLAY_PIN_MOSI  19    /* Camera Y4 */
#define DISPLAY_PIN_RST   13    /* Freed SDMMC DAT3 in 1-bit mode */

/**
 * @brief Initialize the Nokia C1-01 display subsystem on SPI3_HOST (VSPI).
 * 
 * Hardware-safe: If the physical panel is not yet connected, SPI transactions
 * transmit into the air without blocking or throwing errors.
 * 
 * @return ESP_OK on successful SPI bus and driver initialization, error otherwise.
 */
esp_err_t display_manager_init(void);

/**
 * @brief Display the boot splash screen with project title and hardware specs.
 */
void display_manager_show_splash(void);

/**
 * @brief Update the display with live playback information.
 * 
 * @param title Track title or file name
 * @param status Playback status ("PLAYING", "PAUSED", "STOPPED")
 * @param sample_rate Audio sampling rate in Hz (e.g., 44100, 96000)
 * @param volume Current volume level (0 to 100)
 */
void display_manager_update(const char *title, const char *status, uint32_t sample_rate, uint8_t volume);

/**
 * @brief Clear the display screen to black.
 */
void display_manager_clear(void);

/**
 * @brief Draw a diagnostic test pattern (color bars and orientation boxes).
 */
void display_manager_test_pattern(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_MANAGER_H */
