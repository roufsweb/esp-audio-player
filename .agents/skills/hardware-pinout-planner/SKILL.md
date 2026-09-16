---
name: hardware-pinout-planner
description: Pinout multiplexing and peripheral conflict verification for ESP32-CAM (HW-297) board reclamation (SDMMC vs PSRAM vs Camera vs Display).
---

# Hardware Pinout Planner Skill

This skill provides verified hardware data and pin conflict management for the ESP32-CAM (HW-297 / ESP-32S) platform used in the ESP Audio Player project.

## 1. Hardware Architecture Overview (HW-297)

### Core Components
- SoC: Espressif ESP32-D0WDQ6 inside Ai-Thinker ESP-32S module.
- Flash: 8MB SPI Flash (DIO mode, uses internal SPI pins GPIO6-11).
- PSRAM: ESP_PSRAM64H (64Mbit / 8MB) connected via internal QSPI (GPIO16 and GPIO17 used for CS/CLK).
- Voltage Regulator: AMS1117 3.3V LDO.

### Pin Categories & Critical Constraints
1. Internal Flash Pins (Never Use):
   - GPIO6, GPIO7, GPIO8, GPIO9, GPIO10, GPIO11.
2. Internal PSRAM Pins (Reserved if PSRAM enabled):
   - GPIO16 (PSRAM CS), GPIO17 (PSRAM CLK).
3. Strapping Pins (Must respect boot levels):
   - GPIO0: Must be HIGH for normal SPI boot, LOW for UART download mode.
   - GPIO2: Must be LOW or floating during boot; tied to SD Data 0 and on-board pull-down.
   - GPIO12: MTDI strapping pin; boot voltage level (must be LOW at boot for 3.3V flash LDO).
   - GPIO15: MTDO strapping pin; controls boot messages over UART.
4. On-board Peripherals on Standard ESP32-CAM:
   - GPIO4: Flashlight / High-power LED (also SD Data 1 in 4-bit SDMMC).
   - GPIO33: Small inverted status LED.
   - MicroSD Slot (built-in): Uses GPIO14 (CLK), GPIO15 (CMD), GPIO2 (DAT0), GPIO4 (DAT1), GPIO12 (DAT2), GPIO13 (DAT3).
   - Camera Header Pins (reclaimable when camera is detached):
     - GPIO32 (XCLK), GPIO0 (SIOC), GPIO26 (SIOD), GPIO27 (VSYNC), GPIO25 (HREF), GPIO21 (PCLK).
     - Camera data bus: GPIO5 (Y2), GPIO18 (Y3), GPIO19 (Y4), GPIO36 (Y5/VP), GPIO39 (Y6/VN), GPIO34 (Y7), GPIO35 (Y8), GPIO12 (Y9).
     - Note: GPIO34, 35, 36, 39 are input-only pins (no internal pull-up/pull-down or output drivers).

## 2. Pinout Conflict Check Protocol
When assigning pins for the Display (SPI: MOSI, SCK, CS, DC, RST) or user controls (buttons, rotary encoder):
1. Check if the pin is Input-Only (GPIO 34, 35, 36, 39 can only be used as inputs, e.g., ADC, buttons with external pull-ups).
2. Check if the pin conflicts with the SDMMC bus mode (1-bit vs 4-bit):
   - 1-bit SDMMC mode frees GPIO4, GPIO12, and GPIO13 for other duties.
   - 4-bit SDMMC mode requires GPIO2, 4, 12, 13, 14, 15.
3. Check if the pin affects strapping levels during power-on or reset.
4. Verify physical continuity and logic levels with the user before finalizing assignments.
