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
4. On-board Peripherals on HW-297 (Reclaimed):
   - **Camera: PERMANENTLY ELIMINATED.** The OV2640 camera is discarded. All 24 camera pins on the FPC connector/pads are reclaimed for Nokia C1-01 display, user controls, and expansion.
   - **GPIO4 (Flashlight LED & MOSFET):** User desoldered the high-power white LED. Recommended action: Desolder the SOT-23 MOSFET on GPIO4 to remove the gate capacitance and 10kΩ pull-down to GND. GPIO4 then becomes a clean GPIO on the outer header (J1 Pin 8) for Display Reset (`TFT_RST`), Display Backlight PWM (`TFT_BL`), or general I/O.
   - **GPIO33:** Small inverted red status LED.
   - **MicroSD Slot (1-Bit SDMMC Mode):** Uses GPIO14 (CLK), GPIO15 (CMD), GPIO2 (DAT0). Frees GPIO4, GPIO12, and GPIO13.
   - **Nokia C1-01 Display (9-bit SPI):**
     - `TFT_CS`: GPIO 5 (Reclaimed Camera Y2 on FPC)
     - `TFT_SCK`: GPIO 18 (Reclaimed Camera Y3 on FPC, Hardware VSPI SCLK)
     - `TFT_MOSI`: GPIO 19 (Reclaimed Camera Y4 on FPC, Hardware VSPI MOSI)
     - `TFT_RST`: GPIO 4 (outer header J1 Pin 8) or GPIO 13 (outer header J1 Pin 4) or GPIO 21 (FPC)
     - `TFT_BL`: GPIO 4 (outer header J1 Pin 8) or 3.3V rail.
   - **Input-Only Pins:** GPIO 34, 35, 36, 39 (no internal pull-up/down, input only for buttons/ADC).

## 2. Pinout Conflict Check Protocol
When assigning pins for the Display (SPI: MOSI, SCK, CS, DC, RST) or user controls (buttons, rotary encoder):
1. Check if the pin is Input-Only (GPIO 34, 35, 36, 39 can only be used as inputs, e.g., ADC, buttons with external pull-ups).
2. Check if the pin conflicts with the SDMMC bus mode (1-bit mode frees GPIO4, GPIO12, and GPIO13).
3. Check if the pin affects strapping levels during power-on or reset (GPIO 0, 2, 12, 15).
4. Verify physical continuity and logic levels with the user before finalizing assignments.
