# ESP Audio Player Project Context

## Hardware Specifications & Reality
- **Board:** ESP32-CAM (Silkscreen model HW-297) with Ai-Thinker ESP-32S module.
- **Flash:** 8MB SPI Flash (configured in partitions.csv and sdkconfig.defaults as 8MB DIO 40MHz).
- **RAM / PSRAM:**
  - Internal SRAM: 520 KB available on ESP32 silicon.
  - External PSRAM: ESP_PSRAM64H (64Mbit / 8MB) chip is physically populated on the HW-297 board.
  - Build Status: Currently, `# CONFIG_SPIRAM is not set` in sdkconfig. The firmware is currently running purely within internal SRAM.
  - Pin Constraints: PSRAM uses GPIO16 and GPIO17 internally. When PSRAM is enabled in software, these pins cannot be repurposed for external peripherals.

## Project Vision
- A high-end, standalone Bluetooth Audio Player (A2DP Source) utilizing the ESP32-CAM hardware platform.
- The camera interface is eliminated or repurposed to reclaim GPIOs for high-speed SDMMC card reading (FATFS) and a 4-wire SPI color display (running an LVGL UI).
- Audio streaming features high-bitrate codecs: custom-patched SBC XQ (up to 250 bitpool Dual Channel) and future high-resolution codecs.

## Current Phase
- Driver validation for SBC XQ audio streaming over Bluetooth Classic A2DP.
- Audio synthesis: Mathematical 440 Hz Sine wave generator in audio data callback (`bt_app_a2d_data_cb`) to verify throughput and link stability without SD card dependencies.
- Control interface: Interactive UART console with `scan`, `connect`, `disconnect`, `play`, and `pause` commands.
