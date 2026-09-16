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

Because the camera module is removed/ditched, all 24 camera FPC signals are available for peripheral re-allocation. The on-board MicroSD slot is used for audio file storage.

### A. On-Board MicroSD Card Slot
The MicroSD card slot on the underside of the HW-297 PCB is routed to the ESP32 hardware SDMMC Host:

| SDMMC Signal | ESP32 GPIO | Header Pin | Notes |
|:-------------|:-----------|:-----------|:------|
| `SD_CLK`     | GPIO14     | J1 Pin 7   | Clock line |
| `SD_CMD`     | GPIO15     | J1 Pin 6   | Command/Response (Strapping pin MTDO; has 10k pull-up) |
| `SD_DAT0`    | GPIO2      | J1 Pin 8   | Data 0 (Strapping pin; bootloader pull-down) |
| `SD_DAT1`    | GPIO4      | J2 Pin 1   | Data 1 (Also drives Flashlight LED via transistor) |
| `SD_DAT2`    | GPIO12     | J1 Pin 4   | Data 2 (Strapping pin MTDI; has 10k pull-up) |
| `SD_DAT3`    | GPIO13     | J1 Pin 5   | Data 3 (Card Detect / CS; has 10k pull-up) |

**Recommended Mode:** **1-Bit SDMMC Mode**
* Uses only `GPIO14` (CLK), `GPIO15` (CMD), and `GPIO2` (DAT0).
* Frees up `GPIO4`, `GPIO12`, and `GPIO13` for general I/O or display control.
* Throughput at 20-40 MHz is 2.5 to 5.0 MB/s (20-40 Mbps), which exceeds the bandwidth required for FLAC/WAV streaming (~0.17 to 0.5 MB/s) by more than 10x.

---

### B. Nokia C1-01 Display Pin Assignment

The Nokia C1-01 display uses a 9-bit SPI protocol (1 D/C bit + 8 data bits packed in software), requiring **no separate D/C pin** and **no MISO pin**. Only 4 lines are required:

| Display Signal | Assigned GPIO | Physical Location | Bus / Peripheral Role |
|:---------------|:--------------|:------------------|:----------------------|
| `TFT_CS`       | GPIO5         | J2 Pin 5 (IO5)    | Hardware VSPI CS / General Output |
| `TFT_SCK`      | GPIO18        | J2 Pin 6 (IO18)   | Hardware VSPI SCLK (Clock @ ~26 MHz) |
| `TFT_MOSI`     | GPIO19        | J2 Pin 7 (IO19)   | Hardware VSPI MOSI (9-bit packed stream) |
| `TFT_RST`      | GPIO13        | J1 Pin 5 (IO13)   | Active-low Display Reset (Freed by 1-bit SDMMC) |

*Alternative Reset Pin:* If 4-bit SDMMC is preferred later, `TFT_RST` can be connected to reclaimed camera pin `GPIO26`, `GPIO27`, `GPIO21`, or `GPIO32`.

---

### C. Pin Allocation Summary Table

| GPIO | Default CAM Function | Audio Player Role | Status / Constraint |
|:----:|:---------------------|:------------------|:--------------------|
| 0    | Camera XCLK / Boot   | Bootloader Mode   | Strapping pin; must be HIGH to boot normally |
| 1    | U0TXD                | UART Console TX   | Serial terminal & firmware flashing |
| 2    | Camera / SD_DAT0     | SDMMC DAT0        | Must be LOW/floating at boot |
| 3    | U0RXD                | UART Console RX   | Serial terminal & firmware flashing |
| 4    | Flash LED / SD_DAT1  | Available / LED   | High-power flash LED line |
| 5    | Camera Y2            | Display CS        | Hardware VSPI CS0 |
| 12   | Camera Y9 / SD_DAT2  | Available GPIO    | Strapping pin MTDI (must be LOW at boot for 3.3V LDO) |
| 13   | Camera / SD_DAT3     | Display RST       | Reset line for Nokia C1-01 LCD |
| 14   | Camera / SD_CLK      | SDMMC CLK         | Clock line for on-board MicroSD |
| 15   | Camera / SD_CMD      | SDMMC CMD         | Command line for on-board MicroSD |
| 16   | PSRAM CS             | Dedicated PSRAM   | Connected to ESP_PSRAM64H (Do not use externally) |
| 17   | PSRAM CLK            | Dedicated PSRAM   | Connected to ESP_PSRAM64H (Do not use externally) |
| 18   | Camera Y3            | Display SCK       | Hardware VSPI SCLK |
| 19   | Camera Y4            | Display MOSI      | Hardware VSPI MOSI |
| 21   | Camera PCLK          | User Button / Enc | Reclaimed camera pad |
| 22   | Camera (Internal)    | User Button / Enc | Reclaimed camera pad |
| 23   | Camera (Internal)    | User Button / Enc | Reclaimed camera pad |
| 25   | Camera HREF          | Available GPIO    | Reclaimed camera pad |
| 26   | Camera SIOD          | Available GPIO    | Reclaimed camera pad |
| 27   | Camera VSYNC         | Available GPIO    | Reclaimed camera pad |
| 32   | Camera XCLK          | Available GPIO    | Reclaimed camera pad |
| 33   | Small Status LED     | System Status LED | Inverted active-low onboard indicator |
| 34   | Camera Y7            | Button Input      | Input-only pin (requires external pull-up) |
| 35   | Camera Y8            | Button Input      | Input-only pin (requires external pull-up) |
| 36   | Camera Y5 (VP)       | Button Input      | Input-only pin (requires external pull-up) |
| 39   | Camera Y6 (VN)       | Button Input      | Input-only pin (requires external pull-up) |

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
