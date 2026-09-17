# Feature Matrix & Roadmap — ESP32 Audiophile A2DP Player

This document tracks all implemented, active, and roadmap features for the ESP32 Audiophile Audio Player.

---

## 1. Audio Engine & Formats

| Feature | Details | Status | Notes |
| :--- | :--- | :--- | :--- |
| **WAV Playback** | 16-bit uncompressed PCM (44.1 kHz, 48.0 kHz) | Fully Functional | Zero CPU overhead, bit-perfect streaming |
| **FLAC (Standard)** | 16-bit / 44.1 kHz lossless audio | Fully Functional | dr_flac decoder with PSRAM ring buffer |
| **FLAC (High Quality)**| 24-bit / 48.0 kHz lossless audio | Fully Functional | Fixed-point decoding |
| **FLAC (High-Res 96k)**| 24-bit / 96.0 kHz studio masters | Downsampled | Downsampled to 44.1k/48k for A2DP compatibility |
| **Sine Tone Generator**| Diagnostic mathematical tone synthesis | Fully Functional | Available via `tone <freq>` |
| **Audio Seeking / Scrubbing**| Fast-Forward (+10s), Rewind (-10s), Absolute Seek | Fully Functional | `seek <sec>`, `ff [sec]`, `rew [sec]` |
| **MP3 Playback** | Standard MPEG-1/2 Audio Layer III | Planned | Targeted via lightweight `minimp3` |

---

## 2. Bluetooth A2DP & AVRCP

| Feature | Details | Status | Notes |
| :--- | :--- | :--- | :--- |
| **A2DP Source** | Bluedroid A2DP audio streaming | Fully Functional | Non-blocking data callback on Core 0 |
| **SBC XQ (Dual Channel)** | High-bitrate independent channel allocation | Fully Functional | Up to 452–492 kbps, bitpool 38 |
| **Joint Stereo (SBC)** | Standard high-quality A2DP transmission | Fully Functional | 328 kbps, bitpool 53 |
| **Adaptive Bitpool Clamping** | Negotiates the maximum ceiling accepted by sink | Fully Functional | Prevents packet drops and buffer overflows |
| **Rate Alignment** | Negotiates 44.1 kHz natively to bypass stack resampler | Fully Functional | Zero CPU burned in Bluedroid software upsampler |
| **Codec & Bitrate Telemetry** | Real-time reporting of codec, bitpool, kbps in JSON | Fully Functional | Sent via `status_json` and `status` to console and Web Dashboard |
| **AVRCP Volume Sync** | Bidirectional absolute volume synchronization | Fully Functional | Supported via `esp_avrc_ct_api` |
| **Auto-Reconnect** | Automatically attempts connection to last-paired device | Fully Functional | Handled by `bt_manager` |

---

## 3. Storage & Buffer Architecture

| Feature | Details | Status | Notes |
| :--- | :--- | :--- | :--- |
| **MicroSD (1-bit SDMMC)**| Hardware SDMMC Slot 1 (GPIO 14, 15, 2) @ 20 MHz | Fully Functional | Native DMA multi-block reads |
| **FAT32 Filesystem** | Long filename support (LFN Heap) | Fully Functional | Up to 255 character filenames |
| **PSRAM Audio Ring Buffer** | 512 KB circular ring buffer in external SPI RAM | Fully Functional | Holds ~3.0 seconds of 44.1k 16-bit stereo PCM |
| **Pre-buffer Cushion** | Holds playback until 128 KB (~740 ms) is primed | Fully Functional | Prevents startup glitches and wear-leveling underruns |
| **Decoupled Cores** | Core 1 decodes audio, Core 0 streams Bluetooth | Fully Functional | Prevents RF task starvation |

---

## 4. Display, Visuals & UI

| Feature | Details | Status | Notes |
| :--- | :--- | :--- | :--- |
| **Nokia C1-01 Display** | 128x160 RGB565 LCD on SPI3 (CS=5, SCK=18, MOSI=19, RST=13) | Driver Verified | SPI host initialized, splash screen rendered |
| **Now Playing UI** | Track name, artist, elapsed/total time, progress bar | Planned | Next milestone |
| **UI Animations** | Smooth transitions, volume overlay, status indicators | Planned | See `.agents/skills/ui-animation-design/` |
| **Synchronized Lyrics** | Real-time scrolling `.lrc` timestamped lyrics | Planned | Next milestone |
| **Album Art** | 128x128 thumbnail rendering from SD card | Planned | Milestone |

---

## 5. Web Serial Dashboard

| Feature | Details | Status | Notes |
| :--- | :--- | :--- | :--- |
| **Web Serial Connection**| 115200 baud direct browser connection (Chrome/Edge) | Fully Functional | `index.html` |
| **Real-time Telemetry** | State, sample rate, bit depth, SRAM, PSRAM, buffer | Fully Functional | Polled via `status_json` |
| **File Browser** | Live SD card directory browsing and click-to-play | Fully Functional | Built via `ls_json` |
| **Bluetooth Codec Badge** | Live display of SBC XQ / SBC, bitpool, and bitrate | Fully Functional | Integrated into `index.html` with stream telemetry box |
| **Scrubber & Fast-Forward**| Interactive progress bar and +10s / -10s skip buttons | Fully Functional | Click-to-seek, +10s FF, -10s REW in `index.html` |
