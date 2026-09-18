# Hardware Reference: ESP Audio Player

This document details verified hardware specifications, module pinouts, peripheral connections, and system resource calculations for the ESP Audio Player project.

---

## 1. Board Identity & Physical Architecture

The hardware platform is an **ESP32-CAM (silkscreen HW-297)** module.

Reference image: `hardware/esp32cam_hw297_top.png`

```
+-------------------------------------------------------------+
|                          ANTENNA                            |
|                     [PCB Trace]  (IPEX)                     |
|                                                             |
|                   +---------------------+                   |
|                   |       ESP-32S       |                   |
|   [Pin Header]    |     WiFi + BT SoC   |    [Pin Header]   |
|   (8 Pins, J1)    |   FCC: 2BCLP-ESP-32S|    (8 Pins, J2)   |
|                   |                     |                   |
|                   +---------------------+                   |
|                                                             |
|   [Tantalum Cap]      [LED1]                                |
|                                       +-----------------+   |
|   [RST Button]     +-------------+    | ESP_PSRAM64H    |   |
|                    | AMS1117 3.3 |    | (8MB PSRAM)     |   |
|                    +-------------+    +-----------------+   |
+-------------------------------------------------------------+
```

### Verified Components & Chips
* **Microcontroller Module:** Ai-Thinker ESP-32S (housing Espressif ESP32-D0WDQ6, dual-core Xtensa 32-bit LX6 @ 240 MHz).
* **Flash Memory:** 8MB SPI Flash (configured in partitions.csv for 8MB DIO mode @ 40 MHz).
* **External Pseudo-SRAM:** Espressif `ESP_PSRAM64H` (64 Mbit / 8 MByte QSPI, 3.3V).
  * Connected internally to ESP32 pins: `GPIO16` (PSRAM CS) and `GPIO17` (PSRAM CLK).
  * Must remain dedicated to the PSRAM peripheral when `CONFIG_SPIRAM` is enabled.
* **Power Regulation:** Advanced Monolithic Systems `AMS1117-3.3` (5V input to 3.3V rail).
* **Display Peripheral:** Nokia C1-01 LCD (salvaged ST7735 / SPFD54124B controller, 128x160 resolution, 9-bit SPI interface).

---

## 2. Pin Mapping & Peripheral Reclamation

### A. Permanent Camera Elimination
* **Camera Status:** **PERMANENTLY ELIMINATED / EXCLUDED.**
  * The OV2640 camera module is removed and will not be used in this project.
  * All 24 camera signals (FPC connector and internal pads) are permanently reclaimed for the **Nokia C1-01 LCD display**, user input controls (rotary encoder, buttons), and future expansion.
  * Dedicated camera clocks (XCLK) and power management (PWDN) are completely deactivated.

### B. High-Power Flashlight LED & MOSFET Modification (GPIO 4)
* **Flashlight LED:** Desoldered by the user.
* **MOSFET Removal Recommendation:** **REMOVE (DESOLDER) THE MOSFET.**
  * **Why:** The gate of the SOT-23 N-channel MOSFET driving the flashlight LED is directly connected to **GPIO 4**. It includes an on-board **10kΩ pull-down resistor to GND** and introduces parasitic gate capacitance (~50–100 pF).
  * **Benefit of Removal:** Removing the 3-pin SOT-23 MOSFET (or clipping its gate pin) completely isolates GPIO 4, eliminating the 10kΩ pull-down load and gate capacitance.
  * **Result:** **GPIO 4 becomes a 100% clean, standard GPIO** on the outer 2.54 mm header (J1 / Pin 8), ideal for:
    * Nokia C1-01 Display Reset (`TFT_RST`)
    * Display Backlight PWM brightness control (`TFT_BL`)
    * User button or rotary encoder input
    * Or future 4-bit SDMMC (`DAT1` with clean pull-up)

---

### C. On-Board MicroSD Card Slot
The MicroSD card slot on the underside of the HW-297 PCB is routed to the ESP32 hardware SDMMC Host:

| SDMMC Signal | ESP32 GPIO | Physical Pin | Notes |
|:-------------|:-----------|:-------------|:------|
| `SD_CLK`     | GPIO14     | J1 Pin 6     | Hardware SDMMC Clock line |
| `SD_CMD`     | GPIO15     | J1 Pin 5     | Command/Response (Strapping pin MTDO; has 10k pull-up) |
| `SD_DAT0`    | GPIO2      | J1 Pin 7     | Data 0 (Strapping pin; bootloader pull-down) |
| `SD_DAT1`    | GPIO4      | J1 Pin 8     | Data 1 (Flashlight line; freed by removing LED/MOSFET) |
| `SD_DAT2`    | GPIO12     | J1 Pin 3     | Data 2 (Strapping pin MTDI; must be LOW at boot) |
| `SD_DAT3`    | GPIO13     | J1 Pin 4     | Data 3 (Card Detect / CS; has 10k pull-up) |

**Active Operating Mode:** **1-Bit SDMMC Mode**
* Uses only `GPIO14` (CLK), `GPIO15` (CMD), and `GPIO2` (DAT0).
* Frees up `GPIO4`, `GPIO12`, and `GPIO13` for display and general I/O.

---

### D. Nokia C1-01 Display Pin Assignments (100% Reclaimed Camera FPC)

The Nokia C1-01 display uses a 9-bit SPI protocol (1 D/C bit + 8 data bits packed in software), requiring **no separate D/C pin** and **no MISO pin**. The entire display (power, ground, SPI bus, reset, and backlight) can be powered and controlled directly from the **24-pin Camera Connector (`Cam1`)**:

| Display Signal | Assigned GPIO | FPC Physical Pin | Bus / Peripheral Role | Overlap / Conflict Status |
|:---------------|:--------------|:-----------------|:----------------------|:--------------------------|
| **`TFT_CS`**   | **GPIO 5**    | `Cam1` Pin 6 (`CSI_D0`) | Hardware VSPI CS0 | **Zero conflicts** |
| **`TFT_SCK`**  | **GPIO 18**   | `Cam1` Pin 4 (`CSI_D1`) | Hardware VSPI SCLK (~26 MHz) | **Zero conflicts** |
| **`TFT_MOSI`** | **GPIO 19**   | `Cam1` Pin 3 (`CSI_D2`) | Hardware VSPI MOSI (9-bit packed) | **Zero conflicts** |
| **`TFT_RST`**  | **GPIO 21**   | `Cam1` Pin 5 (`CSI_D3`) | Active-low Display Reset | **Zero conflicts (SDMMC DAT3 freed!)** |
| **`TFT_BL`**   | **GPIO 22**   | `Cam1` Pin 8 (`CSI_PCLK`)| Optional Backlight PWM Dimming | **Zero conflicts** (or tie to 3.3V) |
| **`TFT_VCC`**  | **3.3V**      | `Cam1` Pin 14 (`DOVDD`) | Main 3.3V Power Rail | Filtered by on-board `C14` (0.1µF) |
| **`TFT_GND`**  | **GND**       | `Cam1` Pin 10 (`DGND`)  | System Ground | Direct PCB Ground |

*Key Advantage:* Connecting the display to the camera connector completely isolates the display from the MicroSD card lines, allowing full **4-Bit SDMMC Mode** on GPIO 2, 4, 12, 13, 14, 15 without any line contention!

---

### E. Pin Allocation Summary Table

| GPIO | Default CAM Function | Audio Player Role | Physical Location | Status / Constraint |
|:----:|:---------------------|:------------------|:------------------|:--------------------|
| 0    | Camera XCLK / Boot   | Bootloader Mode   | Outer Header J2   | Strapping pin; must be HIGH to boot normally |
| 1    | U0TXD                | UART Console TX   | Outer Header J2   | Serial terminal & firmware flashing |
| 2    | Camera / SD_DAT0     | SDMMC DAT0        | Outer Header J1   | Must be LOW/floating at boot |
| 3    | U0RXD                | UART Console RX   | Outer Header J2   | Serial terminal & firmware flashing |
| 4    | Flash LED / SD_DAT1  | **Display RST / BL** | Outer Header J1 | Clean GPIO after removing LED & MOSFET |
| 5    | Camera Y2            | **Display CS**    | Reclaimed CAM FPC | Hardware VSPI CS0 |
| 12   | Camera Y9 / SD_DAT2  | Available GPIO    | Outer Header J1   | Strapping pin MTDI (must be LOW at boot) |
| 13   | Camera / SD_DAT3     | Available / RST   | Outer Header J1   | Freed in 1-bit SD mode |
| 14   | Camera / SD_CLK      | SDMMC CLK         | Outer Header J1   | Clock line for on-board MicroSD |
| 15   | Camera / SD_CMD      | SDMMC CMD         | Outer Header J1   | Command line for on-board MicroSD |
| 16   | PSRAM CS             | Dedicated PSRAM   | Internal Module   | Connected to ESP_PSRAM64H (DO NOT USE) |
| 17   | PSRAM CLK            | Dedicated PSRAM   | Internal Module   | Connected to ESP_PSRAM64H (DO NOT USE) |
| 18   | Camera Y3            | **Display SCK**   | Reclaimed CAM FPC | Hardware VSPI SCLK |
| 19   | Camera Y4            | **Display MOSI**  | Reclaimed CAM FPC | Hardware VSPI MOSI |
| 21   | Camera PCLK          | User Button / Enc | Reclaimed CAM FPC | Reclaimed general output/input |
| 22   | Camera (Internal)    | User Button / Enc | Reclaimed CAM FPC | Reclaimed general output/input |
| 23   | Camera (Internal)    | User Button / Enc | Reclaimed CAM FPC | Reclaimed general output/input |
| 25   | Camera HREF          | Available GPIO    | Reclaimed CAM FPC | Reclaimed general output/input |
| 26   | Camera SIOD          | Available GPIO    | Reclaimed CAM FPC | Reclaimed general output/input |
| 27   | Camera VSYNC         | Available GPIO    | Reclaimed CAM FPC | Reclaimed general output/input |
| 32   | Camera XCLK          | Available GPIO    | Reclaimed CAM FPC | Reclaimed general output/input |
| 33   | Small Status LED     | System Status LED | Onboard Red LED   | Inverted active-low indicator |
| 34   | Camera Y7 / J2-5     | **Battery ADC1**  | Header J2 Pin 5   | ADC1_CH6: Li-Ion 2:1 divider (active with BT) |
| 35   | Camera Y8 / J2-4     | **Rotary Phase A**| Header J2 Pin 4   | Input-only digital input |
| 36   | Camera Y5 (VP)       | Reclaimed CAM FPC | FPC Pin 15        | Input-only pin (ADC1_CH0) |
| 39   | Camera Y6 (VN)       | **Rotary Phase B**| FPC Pin 13        | Input-only digital input (Sensor VN) |

---

## 3. System Resource & Performance Budget

### A. Memory Allocation (Internal SRAM vs 8MB PSRAM)

```
+-------------------------------------------------------------------------+
| TOTAL MEMORY: 520 KB Internal SRAM + 8192 KB External PSRAM            |
+-------------------------------------------------------------------------+
| INTERNAL SRAM (520 KB):                                                 |
| - Bluetooth Controller (Classic BT Baseband / Link Mgr):    ~60 KB      |
| - Bluedroid Stack & SBC Encoder Context:                    ~80 KB      |
| - FreeRTOS System & Core Task Stacks:                       ~35 KB      |
| - DMA Buffers (SDMMC + Nokia Display SPI):                  ~16 KB      |
| - Free Internal SRAM Headroom:                             ~130 KB      |
+-------------------------------------------------------------------------+
| EXTERNAL PSRAM (8192 KB):                                               |
| - Audio File Read-Ahead Ring Buffer (5-10 seconds audio):  1024 KB      |
| - Decoded PCM Output Ring Buffer (SBC XQ feeder):           256 KB      |
| - ID3 Album Art JPEG/PNG Decode & Cache Buffer:             256 KB      |
| - FLAC / AAC / MP3 Decoder Working Scratchpad:              128 KB      |
| - Display Frame Cache (128x160 RGB565 = 40 KB x 2):         80 KB      |
| - LVGL UI Objects & Font Tables:                            256 KB      |
| - Free PSRAM Headroom:                                    ~6192 KB      |
+-------------------------------------------------------------------------+
```

### B. Dual-Core CPU Budgeting (@ 240 MHz)

| Core | Primary Responsibilities | Worst-Case CPU Utilization |
|:-----|:-------------------------|:---------------------------|
| **Core 0 (Protocol Core)** | Classic Bluetooth Controller baseband, Bluedroid host stack, A2DP Source state machine, SBC XQ real-time encoder (bitpool 250 Dual Channel). | **28% - 35%** |
| **Core 1 (Application Core)** | SDMMC FATFS streaming, Audio decoders (MP3/WAV/FLAC/AAC), ID3 tag and JPEG album art scaling, Nokia C1-01 9-bit SPI driver, UI event loop. | **35% - 48%** (during active decode & redraw) |

Both cores operate with **over 50% idle headroom**, preventing audio underruns or UI freezes.

---

## 4. Audio Decoding & Album Art Pipeline

1. **SDMMC Storage Reader Task (`task_file_reader`):**
   * Reads raw bytes from MicroSD via FATFS in 4096-byte clusters.
   * Feeds the 1 MB PSRAM raw stream buffer.
2. **Audio Decoder Task (`task_audio_decoder`):**
   * Automatically identifies container (WAV, MP3, FLAC, AAC).
   * Parses ID3v2 tags; extracts embedded APIC (Attached Picture) frame for album art.
   * Decodes audio frames into 16-bit 44.1 kHz stereo PCM chunks.
   * Feeds the 256 KB PCM ring buffer.
3. **Bluetooth A2DP Source Callback (`bt_app_a2d_data_cb`):**
   * Pulls PCM frames directly from the PCM ring buffer.
   * Encodes audio via SBC XQ (Dual Channel, bitpool 250) at ~453-552 kbps.
   * Zero audio dropouts due to multi-second read-ahead buffering.
4. **Album Art Processing:**
   * Scaled from source resolution (e.g. 300x300 or 500x500) directly to 128x128 or 128x160 using `TJpgDec` with 1/2 or 1/4 downsampling.
   * Pushed directly to the Nokia C1-01 display using the optimized 9-bit SPI driver.
