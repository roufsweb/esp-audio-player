#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sd_protocol_types.h"


#ifdef __cplusplus
extern "C" {
#endif

#define SD_CARD_MOUNT_POINT "/sdcard"



/**
 * @brief Initialize 1-bit hardware SDMMC interface and mount FATFS to /sdcard.
 *
 * Configures Slot 1 with 1-bit bus width:
 *   CLK  -> GPIO14
 *   CMD  -> GPIO15
 *   DAT0 -> GPIO2
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL or error code on mount failure
 */
esp_err_t sd_card_init(void);

/**
 * @brief Unmount FATFS and deinitialize the SDMMC host.
 *
 * @return
 *      - ESP_OK on success
 */
esp_err_t sd_card_deinit(void);

/**
 * @brief Check if the SD card is currently mounted and accessible.
 *
 * @return true if mounted, false otherwise
 */
bool sd_card_is_mounted(void);

/**
 * @brief Get the pointer to the initialized sdmmc_card_t structure.
 *
 * @return const pointer to sdmmc_card_t, or NULL if not mounted
 */
const sdmmc_card_t *sd_card_get_info(void);

/**
 * @brief Register storage and system diagnostic console commands:
 *        - ls [path]    : List directory contents with sizes and LFN support
 *        - cat <path>   : Read and dump initial bytes of a file
 *        - sdinfo       : Display SD card CID, CSD, speed, and capacity
 *        - mem          : Display detailed heap (SRAM, PSRAM, DMA) diagnostics
 */
void sd_card_register_console_commands(void);

#ifdef __cplusplus
}
#endif
