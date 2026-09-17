# ESP Audio Player

An embedded Bluetooth Audio Transmitter (A2DP Source) implemented on the ESP32 platform using the ESP-IDF framework.

The firmware streams audio to Bluetooth headphones and speakers using a custom SBC XQ implementation (Dual Channel mode with a maximum bitpool of 250) for high audio fidelity. The player reads multi-format audio files from an on-board MicroSD card, renders track metadata and album art to a salvaged Nokia C1-01 color display, and utilizes an external 8MB PSRAM buffer to ensure continuous playback.

---

## Current Status

* **Bluetooth Stack:** ESP-IDF Bluedroid classic Bluetooth A2DP Source.
* **Codec Engine:** Patched `bta_av_co.c` implementing adaptive SBC negotiation: Dual Channel (SBC XQ @ ~452–492 kbps, bitpool 38) for audiophile fidelity, with universal Joint Stereo fallback (328 kbps, bitpool 53). Prioritizes 44.1 kHz native feeding to bypass Bluedroid software upsampling.
* **Audio Playback:** Lossless FLAC (16-bit / 24-bit, 44.1k/48k/96k) via `dr_flac` and uncompressed WAV PCM, with seek, fast-forward (+10s), and rewind (-10s).
* **Audio Synthesis:** Real-time stereo sine wave generator synthesized mathematically at 44.1 kHz, 16-bit (`tone <freq>`).
* **Control Interface:** Interactive UART command-line interface (`esp_console`) and Web Serial Dashboard (`index.html`) with real-time Bluetooth codec, bitrate, and timeline scrubbing.
* **Hardware Target:** ESP32-CAM (silkscreen HW-297) with an Ai-Thinker ESP-32S module, 8MB SPI Flash, and an on-board ESP_PSRAM64H (8MB) PSRAM chip.
* **Display Target:** Nokia C1-01 LCD (128x160 resolution, 9-bit SPI, ST7735 controller).
* **Storage Target:** Built-in MicroSD slot on the HW-297 board via hardware SDMMC (1-bit mode @ 20 MHz).

For detailed hardware specifications, pin tables, and memory calculations, see [HARDWARE.md](HARDWARE.md). For feature roadmap and driver limits, see [FEATURES.md](FEATURES.md).

---

## Supported Audio Formats & File Specifications

### Audio Formats Matrix

| Format | Bit Depth | Sample Rates | Channels | Playback Engine | Throughput / Overhead |
|:---|:---|:---|:---|:---|:---|
| **FLAC (Standard Lossless)** | 16-bit | 44.1 kHz, 48.0 kHz | Stereo & Mono | `dr_flac` streaming decoder | Native bit-perfect decoding into 512 KB PSRAM ring buffer |
| **FLAC (High Quality Lossless)** | 24-bit | 44.1 kHz, 48.0 kHz | Stereo & Mono | `dr_flac` S32 fixed-point | Decoded via 32-bit arithmetic, scaled to 16-bit for A2DP |
| **FLAC (Studio Master)** | 24-bit | 88.2 kHz, 96.0 kHz | Stereo & Mono | `dr_flac` + integer decimation | Decoded and downsampled to 44.1k / 48k for A2DP compatibility |
| **WAV (Linear PCM)** | 16-bit | 44.1 kHz, 48.0 kHz | Stereo & Mono | Native RIFF parser | Zero CPU decode overhead; direct SD-to-ring-buffer DMA streaming |
| **Diagnostic Sine Tone** | 16-bit | 44.1 kHz | Stereo | Mathematical synthesis | Generates pure test tone (`tone <freq>`) without SD card |
| **MP3 (MPEG-1/2 Layer III)** | 16-bit | 32.0–48.0 kHz | Stereo & Mono | `minimp3` (Roadmap) | Lightweight fixed-point decoder scheduled for next phase |

### File Size Guidelines & Performance

* **Standard File Sizes (10 MB to 60 MB):** 
  Standard 3 to 6-minute CD-quality FLAC and WAV tracks (15 MB–45 MB) play with instant pre-buffering (~740 ms cushion) and uninterrupted playback.
* **Large File Sizes (>100 MB):**
  Files over 100 MB (such as high-res 24-bit/96kHz master files or full live albums) are supported via the 512 KB PSRAM ring buffer. To ensure smooth playback of very large lossless files:
  1. **Format SD Card as FAT32 with 16 KB or 32 KB Cluster Size (Allocation Unit Size):** 
     Default 4 KB clusters cause excessive FAT lookup fragmentation during sustained multi-megabyte reads. Formatting the MicroSD card with **16 KB (`16384` bytes)** or **32 KB** clusters aligns directly with the hardware SDMMC driver multi-block read chunks, doubling SD throughput.
  2. **Card Speed Class:** Use Class 10 / UHS-I U1 or higher MicroSD cards.

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
| `play_file` | `<path>` | Plays a lossless FLAC or WAV file from SD card |
| `seek` | `<seconds>` | Seeks directly to an absolute timestamp in the track |
| `ff` | `[seconds]` | Fast forwards playback by specified delta (default +10s) |
| `rew` | `[seconds]` | Rewinds playback by specified delta (default -10s) |
| `volume` | `[percentage]` | Gets or sets digital logarithmic volume (0–100%) |
| `status` | None | Comprehensive playback state, format, time, and BT codec/bitrate |
| `status_json` | None | Compact JSON telemetry for Web Serial Dashboard |
| `scan_json` | None | Discovered Bluetooth sinks as JSON array |
| `ls_json` | `[path]` | Directory listing as JSON for Web UI file browser |
| `ls` | `[path]` | Lists files and directories on SD card with size and LFN |
| `cat` | `<path> [bytes]`| Dumps initial bytes of a file on SD card |
| `sdinfo` | None | Displays SD card hardware CID/CSD metadata and capacity |
| `mem` | None | Displays internal SRAM, external PSRAM, and DMA heap statistics |
| `benchmark_audio`| `<path> [sec]` | Runs micro-benchmark isolating SDMMC, FLAC S32/S16 decode, and resamplers |
| `restart` | None | Software reboots the ESP32 |

---

## Web Serial Dashboard

A browser-based management dashboard (`index.html`) connects directly to the ESP32 UART over Web Serial (Google Chrome / Microsoft Edge):

* **Live Playback Controls:** Play, Pause, Stop, Fast-Forward (+10s), Rewind (-10s), and click-to-seek progress scrubber.
* **Bluetooth Audio Sink Management:** GAP device scanning, one-click connection, auto-reconnect, and disconnection.
* **Real-time Codec & Bitrate Reporting:** Live badge showing negotiated profile (e.g. `SBC XQ (Dual Ch) @ 452 kbps`, bitpool 38, 44.1 kHz).
* **Interactive SD File Browser:** Live directory browsing with single-click file launch.
* **Perceptual Volume:** Logarithmic slider synchronized via AVRCP Absolute Volume.

---

## Project Roadmap

1. **Step 1: Link & Codec Validation (Complete)**
   * Confirm SBC XQ negotiation (Dual Channel, bitpool 38 @ ~452 kbps) and adaptive Joint Stereo fallback.
   * Verify console REPL controls.

2. **Step 2: External PSRAM Activation & SDMMC Driver (Complete)**
   * Enable `CONFIG_SPIRAM=y` in `sdkconfig.defaults` to initialize the on-board 8MB `ESP_PSRAM64H`.
   * Initialize 1-bit SDMMC driver on pins `GPIO14` (CLK), `GPIO15` (CMD), and `GPIO2` (DAT0).
   * Mount FATFS with long filename (LFN) support and verify file directory listing via `ls` / `cat`.
   * Implement real-time heap diagnostics (`mem`).

3. **Step 3: Multi-Format Audio Decoding Pipeline (Complete)**
   * WAV (uncompressed PCM) and FLAC (dr_flac 16-bit / 24-bit) decoders fully implemented.
   * 512 KB PSRAM circular ring buffer with 128 KB pre-buffering cushion on dedicated Core 1 decode task.
   * Seeking and fast-forward/rewind support (`seek`, `ff`, `rew`).

4. **Step 4: ID3 Album Art Extraction & Nokia C1-01 Display Integration (Next)**
   * Port the 9-bit SPI driver from `E:\rouf\hardware-project\nokia-c1-01-display-driver` as a project component.
   * Wire Nokia C1-01 LCD: CS (`GPIO5`), SCK (`GPIO18`), MOSI (`GPIO19`), RST (`GPIO13`).
   * Extract embedded ID3 JPEG album art and scale to 128x128 using `TJpgDec`.
   * Render playback progress, track title, artist, and album art with UI animations per `.agents/skills/ui-animation-design/`.
