#include "display_manager.h"
#include "Nokia_C1_01_HardwareSPI.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISP_MGR";

/* Colors (RGB565) */
#define C_BLACK   0x0000u
#define C_WHITE   0xFFFFu
#define C_RED     0xF800u
#define C_GREEN   0x07E0u
#define C_BLUE    0x001Fu
#define C_CYAN    0x07FFu
#define C_MAGENTA 0xF81Fu
#define C_YELLOW  0xFFE0u
#define C_DARKGREY 0x39E7u
#define C_ACCENT  0x05BFu /* Neon Cyan */

/* Static driver instance on SPI3_HOST (VSPI) */
static Nokia_C1_01_HardwareSPI s_disp(
    SPI3_HOST,
    DISPLAY_PIN_CS,
    DISPLAY_PIN_SCK,
    DISPLAY_PIN_MOSI,
    DISPLAY_PIN_RST
);

static bool s_initialized = false;

esp_err_t display_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Nokia C1-01 LCD on SPI3_HOST (CS=%d, SCK=%d, MOSI=%d, RST=%d)...",
             DISPLAY_PIN_CS, DISPLAY_PIN_SCK, DISPLAY_PIN_MOSI, DISPLAY_PIN_RST);

    /* 26 MHz SPI clock, chunk_lines=2, internal SRAM buffer */
    bool ok = s_disp.begin(26000000UL, 2, false);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to initialize Nokia C1-01 display driver");
        return ESP_FAIL;
    }

    s_disp.setRotation(0); /* Portrait: 128 wide x 160 tall */
    s_disp.fillScreen(C_BLACK);
    s_disp.flush();

    s_initialized = true;
    ESP_LOGI(TAG, "Nokia C1-01 display driver initialized successfully (128x160 RGB565).");

    display_manager_show_splash();
    return ESP_OK;
}

void display_manager_show_splash(void)
{
    if (!s_initialized) return;

    s_disp.fillScreen(C_BLACK);

    /* Header Bar */
    s_disp.fillRect(0, 0, 128, 18, C_ACCENT);

    /* Accent decoration */
    s_disp.fillRect(10, 30, 108, 2, C_WHITE);

    /* Album Art / Center Placeholder Box */
    s_disp.fillRect(24, 42, 80, 80, C_DARKGREY);
    s_disp.drawFastHLine(24, 42, 80, C_CYAN);
    s_disp.drawFastHLine(24, 121, 80, C_CYAN);
    s_disp.drawFastVLine(24, 42, 80, C_CYAN);
    s_disp.drawFastVLine(103, 42, 80, C_CYAN);

    /* Bottom Volume Deck */
    s_disp.fillRect(14, 136, 100, 6, C_DARKGREY);
    s_disp.fillRect(14, 136, 75, 6, C_ACCENT);

    s_disp.flush();
    ESP_LOGI(TAG, "Display splash screen rendered.");
}

void display_manager_update(const char *title, const char *status, uint32_t sample_rate, uint8_t volume)
{
    if (!s_initialized) return;

    /* Top Status Bar: Black with indicator */
    uint16_t status_color = C_WHITE;
    if (status && strcmp(status, "PLAYING") == 0) {
        status_color = C_GREEN;
    } else if (status && strcmp(status, "PAUSED") == 0) {
        status_color = C_YELLOW;
    }

    s_disp.fillRect(0, 0, 128, 16, C_BLACK);
    s_disp.fillRect(4, 4, 8, 8, status_color);

    /* Sample Rate Tag (Badge) */
    uint16_t rate_color = (sample_rate >= 88200) ? C_MAGENTA : C_CYAN;
    s_disp.fillRect(70, 3, 54, 10, rate_color);

    /* Progress & Volume Bar */
    s_disp.fillRect(10, 140, 108, 6, C_DARKGREY);
    if (volume > 0) {
        uint16_t bar_w = (volume > 100) ? 108 : (volume * 108) / 100;
        s_disp.fillRect(10, 140, bar_w, 6, C_ACCENT);
    }

    s_disp.flush();
}

void display_manager_clear(void)
{
    if (!s_initialized) return;
    s_disp.fillScreen(C_BLACK);
    s_disp.flush();
}

void display_manager_test_pattern(void)
{
    if (!s_initialized) return;

    ESP_LOGI(TAG, "Rendering display color bar test pattern...");
    s_disp.fillScreen(C_BLACK);

    /* 6 Color Bars across the screen */
    const uint16_t bars[6] = { C_RED, C_GREEN, C_BLUE, C_YELLOW, C_CYAN, C_MAGENTA };
    int bar_h = 160 / 6;

    for (int i = 0; i < 6; i++) {
        s_disp.fillRect(0, i * bar_h, 128, bar_h, bars[i]);
    }

    /* Corner check boxes */
    s_disp.fillRect(0, 0, 12, 12, C_WHITE);
    s_disp.fillRect(128 - 12, 0, 12, 12, C_WHITE);
    s_disp.fillRect(0, 160 - 12, 12, 12, C_WHITE);
    s_disp.fillRect(128 - 12, 160 - 12, 12, 12, C_WHITE);

    s_disp.flush();
}
