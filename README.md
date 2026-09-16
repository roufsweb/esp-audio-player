# ESP Audio Player

An embedded Bluetooth Audio Transmitter (A2DP Source) implemented on the ESP32 platform using the ESP-IDF framework.

The firmware streams audio to Bluetooth headphones and speakers using a custom SBC XQ implementation (Dual Channel mode with a maximum bitpool of 250) for high audio fidelity. The player reads multi-format audio files from an on-board MicroSD card, renders track metadata and album art to a salvaged Nokia C1-01 color display, and utilizes an external 8MB PSRAM buffer to ensure continuous playback.

---

## Current Status

* **Bluetooth Stack:** ESP-IDF Bluedroid classic Bluetooth A2DP Source.
* **Codec Engine:** Patched `bta_av_co.c` overriding standard SBC negotiation. It forces Dual Channel, 16 blocks, 8 subbands, and raises maximum bitpool from 53 to 250 (~453 to 552 kbps).
* **Audio Synthesis:** Real-time 440 Hz stereo sine wave generator synthesized mathematically at 44.1 kHz, 16-bit to validate throughput without physical storage attached.
* **Control Interface:** Interactive UART command-line interface running on UART0 (`esp_console`).
* **Hardware Target:** ESP32-CAM (silkscreen HW-297) with an Ai-Thinker ESP-32S module, 8MB SPI Flash, and an on-board ESP_PSRAM64H (8MB) PSRAM chip.
* **Display Target:** Nokia C1-01 LCD (128x160 resolution, 9-bit SPI, ST7735 controller).
* **Storage Target:** Built-in MicroSD slot on the HW-297 board via hardware SDMMC.

For detailed hardware specifications, pin tables, and memory calculations, see [HARDWARE.md](HARDWARE.md). For feature roadmap and driver limits, see [FEATURES.md](FEATURES.md).

---

## Repository Structure

```
esp-audio-player/
├── .agents/                          # Development rules and workflow skills
│   ├── rules/
│   │   ├── Core_Engineering_Rules.md  # Fact verification, step-by-step, no bloat
│   │   ├── ESP32_A2DP_Context.md      # Hardware specs, constraints, and scope
│   │   └── ESP_IDF_Coding_Guidelines.md# Error handling, logging, task structure
│   └── skills/
│       ├── embedded-project-workflow/ # Full-stack development execution rules
│       ├── hardware-pinout-planner/   # ESP32-CAM pin conflict management
│       └── github-workflow/           # Commit and branch standards
├── ESP_Audio_Player/                 # Main ESP-IDF project directory
│   ├── CMakeLists.txt                # Project build script with Bluedroid patch hook
│   ├── partitions.csv                # Custom 8MB flash partition layout
│   ├── sdkconfig.defaults            # Bluetooth, PSRAM, and flash baseline settings
│   ├── components/
│   │   └── bt_override/
│   │       └── bta_av_co.c           # Patched Bluedroid AV callout (SBC XQ)
│   ├── main/
│   │   ├── CMakeLists.txt            # Main component registration
│   │   └── main.c                    # GAP discovery, A2DP callbacks, audio synth, REPL
│   └── build/                        # Build output directory
├── hardware/
│   └── esp32cam_hw297_top.png        # Board photograph and component inspection
├── tools/                            # Offline installer and toolchain utilities
├── FEATURES.md                       # Planned features, driver capabilities, and limits
├── HARDWARE.md                       # Complete hardware pinout and resource budget
└── README.md                         # Project documentation
```

---

## Build and Flash

### Prerequisites
* ESP-IDF v5.3.1 installed (e.g., at `C:\Espressif\frameworks\esp-idf-v5.3.1`).
* USB-to-UART serial adapter connected to ESP32 UART0 (`U0TXD` / `U0RXD`, `GND`, `5V`).
* Ensure `GPIO0` is grounded when entering bootloader mode for flashing.

### Compilation
From the `ESP_Audio_Player` directory:

```bash
# Set up ESP-IDF environment (if not already loaded in shell)
. C:\Espressif\frameworks\esp-idf-v5.3.1\export.ps1

# Set target (first-time build)
idf.py set-target esp32

# Build the project
idf.py build
```

### Flashing
```bash
# Flash application, bootloader, and partition table
idf.py -p COM_PORT flash

# Open serial monitor
idf.py -p COM_PORT monitor
```

---

## Console Command Reference

Once booted, the firmware presents a REPL prompt (`esp32>`) over UART0 at 115200 baud.

| Command | Arguments | Description |
|:--------|:----------|:------------|
| `help` | None | Displays available commands and usage hints |
| `scan` | None | Starts Bluetooth GAP inquiry scan for nearby devices |
| `connect` | `<mac_address>` | Connects to an A2DP sink (e.g. `connect 11:22:33:44:55:66`) |
| `disconnect` | `<mac_address>` | Disconnects the active A2DP audio link |
| `play` | None | Resumes active audio streaming |
| `pause` | None | Suspends active audio streaming |
| `stop` | None | Stops audio streaming and rewinds file |
| `play_file` | `<path>` | Plays an uncompressed WAV file from SD card |
| `tone` | `[frequency_hz]` | Generates mathematical sine test tone (default 440 Hz) |
| `status` | None | Displays playback state, file format, duration, and progress |
| `ls` | `[path]` | Lists files and directories on SD card with size and LFN |
| `cat` | `<path> [bytes]`| Dumps initial bytes of a file on SD card |
| `sdinfo` | None | Displays SD card hardware CID/CSD metadata and capacity |
| `mem` | None | Displays internal SRAM, external PSRAM, and DMA heap statistics |


---

## Project Roadmap

1. **Step 1: Link & Codec Validation (Complete)**
   * Confirm SBC XQ negotiation (bitpool 250, Dual Channel) and test audio transmission.
   * Verify console REPL controls.

2. **Step 2: External PSRAM Activation & SDMMC Driver (Complete)**
   * Enable `CONFIG_SPIRAM=y` in `sdkconfig.defaults` to initialize the on-board 8MB `ESP_PSRAM64H`.
   * Initialize 1-bit SDMMC driver on pins `GPIO14` (CLK), `GPIO15` (CMD), and `GPIO2` (DAT0).
   * Mount FATFS with long filename (LFN) support and verify file directory listing via `ls` / `cat`.
   * Implement real-time heap diagnostics (`mem`).

3. **Step 3: Multi-Format Audio Decoding Pipeline (Next)**
   * Implement WAV (uncompressed PCM), MP3 (Helix decoder), and FLAC (Dr_Flac) decoders.
   * Allocate 1MB PSRAM stream read buffer and 256KB decoded PCM ring buffer.
   * Connect decoder output stream to `bt_app_a2d_data_cb`.

4. **Step 4: ID3 Album Art Extraction & Nokia C1-01 Display Integration**
   * Port the 9-bit SPI driver from `E:\rouf\hardware-project\nokia-c1-01-display-driver` as a project component.
   * Wire Nokia C1-01 LCD: CS (`GPIO5`), SCK (`GPIO18`), MOSI (`GPIO19`), RST (`GPIO13`).
   * Extract embedded ID3 JPEG album art and scale to 128x128 using `TJpgDec`.
   * Render playback progress, track title, artist, and album art.
