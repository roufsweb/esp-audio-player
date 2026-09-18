#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Battery Monitoring on GPIO 34 (ADC1 Channel 6 on Header J2 Pin 5)
 * Dedicated input-only pin, operates concurrently with active Bluetooth (ADC1),
 * leaving all 6 capacitive touch pins 100% free for gestures. */
#define BATTERY_ADC_CHANNEL     ADC1_CHANNEL_6
#define BATTERY_ADC_PIN         34

/* 2:1 precision resistor divider (100k + 100k): multiplier = 2.0 */
#define BATTERY_DIVIDER_RATIO   2.0f

/**
 * @brief Initialize the battery ADC1 monitoring channel on GPIO 34.
 * @return ESP_OK on success.
 */
esp_err_t battery_init(void);

/**
 * @brief Read battery terminal voltage in millivolts.
 * @return Battery voltage in mV (e.g. 3700 for 3.70V, 4200 for 4.20V).
 */
uint32_t battery_get_millivolts(void);

/**
 * @brief Calculate estimated battery state-of-charge percentage (0 - 100%).
 * Uses standard Li-Ion 1S discharge curve with linear interpolation.
 * @return Battery percentage (0 to 100).
 */
uint8_t battery_get_percentage(void);

#ifdef __cplusplus
}
#endif
