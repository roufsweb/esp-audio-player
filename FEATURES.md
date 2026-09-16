# FEATURES.md - ESP Audio Player

This document is the authoritative list of planned features, driver capabilities, and system constraints for the ESP Audio Player project. It is updated alongside code changes.

---

## Audio Playback Features

### Supported Audio Formats
| Format | Decoder | Sample Rates | Bit Depth | Notes |
|:-------|:--------|:-------------|:----------|:------|
| WAV (PCM) | Native parser | 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, 192 kHz | 16-bit, 24-bit | Adaptive real-time SRC downsamples to sink target |
| MP3 | Helix fixed-point | 44.1 kHz, 48 kHz | 16-bit | Bit-perfect playback on matched sink rate |
| FLAC | dr_flac single-header | 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, 192 kHz | 16-bit, 24-bit | Fast linear interpolation resampler; zero drift |
| AAC (.m4a / .aac) | Helix AAC | 44.1 kHz, 48 kHz | 16-bit | Must integrate libhelix-aac separately |

**Multi-Rate Engine & Adaptive Sample Rate Conversion (SRC)**:
- High-resolution streams (88.2k, 96k, 176.4k, 192k) are dynamically downsampled to the highest rate negotiated during the Bluetooth handshake (44.1 kHz or 48.0 kHz) using rational-block linear interpolation running on Core 1 with integer arithmetic and sub-microsecond overhead.
- Native 44.1 kHz and 48.0 kHz streams are passed directly (bit-perfect) without resampling when matched to the sink's configuration.

### Bluetooth Audio Transmission: Multi-Bitrate & Handshake Capability Proving
- **Protocol:** Bluetooth Classic A2DP Source (BR/EDR, AVDTP v1.3)
- **Handshake Capability Proving ("Probe the Absolute Best the Device Supports")**:
  - During the AVDTP `AVDTP_DISCOVER` and `AVDTP_GET_CAPABILITIES` exchange upon connection, the Source probes the remote Sink's Service Capabilities to prove the highest supported parameters:
    1. **Sampling Rate Arbitration**: Probes whether the sink supports 48.0 kHz or 44.1 kHz. If 48.0 kHz is supported, it is offered as the primary high-fidelity tier.
    2. **Channel Mode Negotiation**: Evaluates support for Dual Channel (SBC XQ mode) vs Joint Stereo vs Stereo.
    3. **Bitpool Range Probing**: Reads the Sink's advertised `min_bitpool` and `max_bitpool` limits:
       - **Tier 1 (SBC XQ Ultra-High Bitrate)**: If sink supports Dual Channel with bitpool $\ge 127$, configures bitpool to 127–250 (up to ~790–900 kbps, reaching the physical ACL link throughput ceiling).
       - **Tier 2 (High-Bitpool SBC)**: If sink advertises a raised bitpool cap (e.g., bitpool 64, 76, or 96 in Joint Stereo), configures bitpool up to 76 (~450–510 kbps) for CD+ transparency.
       - **Tier 3 (Standard SBC High-Quality)**: Standard consumer sinks capping at bitpool 53 (328 kbps Joint Stereo, 16 blocks, 8 subbands, Loudness allocation).
- **Transport Link Ceiling:** Physical Bluetooth Classic ACL link provides a practical streaming ceiling of ~800–900 kbps with standard DH5 packets.

### Playback Controls (Phase 1-3: UART REPL only)
- `play_file <path>` - Load and play a file from SDMMC
- `pause` - Suspend audio stream
- `play` - Resume suspended stream
- `next` - Advance to next file alphabetically in current directory
- `prev` - Go to previous file alphabetically in current directory
- `ls <path>` - List directory contents
- `mem` - Display internal SRAM and PSRAM usage

### Playback Controls (Phase 4+: Physical buttons)
Buttons on reclaimed camera GPIO input-only pins with external 10k pull-up resistors.
- Play / Pause: GPIO34
- Next track: GPIO35
- Previous track: GPIO36
- Volume up/down: GPIO39 (rotary encoder or two-button split, to be confirmed)

---

## File System & Storage

### MicroSD Card (SDMMC)
- **Interface:** 1-bit SDMMC hardware peripheral (Slot 1)
- **Pins:** CLK GPIO14, CMD GPIO15, DAT0 GPIO2
- **Filesystem:** FAT32 (FATFS with long filename support, up to 255 characters)
- **Max card size:** Limited by FAT32 to 32 GB formatted natively; 64+ GB cards may need exFAT format (not currently enabled)
- **Throughput at 20 MHz:** 2.5 MB/s read - exceeds all audio streaming requirements
- **Throughput at 40 MHz:** 5.0 MB/s read - with high-speed mode SD cards

### Internal Flash Storage (SPIFFS, 4 MB partition)
- UI font bitmap files (pre-rasterized, stored as raw binary arrays)
- Default album art image (displayed when no APIC tag is found in audio file)
- Splash screen image (128x160 RGB565)
- Estimated total asset usage: less than 200 KB

### Playback State Persistence (NVS, 24 KB partition)
- Last played file path
- Last playback position (byte offset in file)
- Volume level preference

---

## Display Features

### Nokia C1-01 LCD (Nokia display driver)
- **Controller:** ST7735 / SPFD54124B
- **Resolution:** 128 x 160 pixels
- **Color depth:** 16-bit RGB565
- **Interface:** 9-bit Hardware SPI (no D/C pin, no MISO)
- **Pins:** CS GPIO5, SCK GPIO18, MOSI GPIO19, RST GPIO13
- **SPI host:** SPI3_HOST (VSPI, native IOMUX)
- **Max clock:** 26 MHz (limited by the panel, per driver)
- **DMA buffer:** approximately 1.2 KB in internal SRAM (driver requirement)

### Screen Layout (portrait orientation: 128 wide x 160 tall)
```
+------------------+
|                  |   Row 0-127:
|   Album Art      |   128 x 128 pixels, RGB565
|   128 x 128 px   |
|                  |
+------------------+
|  Track Title     |   Row 128-143: 16px high (scrolling if long)
+------------------+
|  Artist Name     |   Row 144-159: 16px high (scrolling if long)
+------------------+
```

### Album Art
- Source: Embedded ID3v2 APIC frame (JPEG)
- Decoder: TJpgDec (streaming callback, no full image buffer required)
- Scaling: TJpgDec decodes at 1/2 scale into a PSRAM staging buffer, then a simple nearest-neighbour scale step brings to 128x128
- Fallback: Default image from SPIFFS when no APIC is found
- No album art from ID3v1 tags (ID3v1 does not support APIC)

### Text Rendering
- Bitmapped fonts stored in SPIFFS
- Long track and artist names scroll horizontally at one pixel per frame update
- No hardware text acceleration; all glyph blitting is CPU-driven

---

## System Architecture Features

### PSRAM Usage (ESP_PSRAM64H, 8 MB)
| Buffer | Size | Purpose |
|:-------|:-----|:--------|
| Raw file stream ring buffer | 1,024 KB | Read-ahead from SDMMC; absorbs seek latency |
| Decoded PCM ring buffer | 256 KB | Feeds A2DP source callback; holds approx. 1.5 sec |
| Album art JPEG staging | 128 KB | TJpgDec intermediate scaled output |
| Decoder working memory | 128 KB | FLAC/AAC internal buffers and history |
| Display frame cache | 40 KB | 128x160 RGB565 frame staging |
| Remaining free | >6,100 KB | Available for future features |

### Dual-Core Task Assignment
| Core | Tasks | Approx. Load |
|:-----|:------|:-------------|
| Core 0 | BT controller, Bluedroid, A2DP, SBC XQ encoder | 30-47% at 160 MHz, 22-32% at 240 MHz |
| Core 1 | SDMMC file reader, audio decoder, ID3 parser, display driver | 35-55% at 160 MHz |

---

## Driver Opportunities

The following are areas where a custom driver improvement would yield measurable benefit:

### DR-1: Sample Rate Converter (SRC), 48 kHz to 44.1 kHz
- **Why:** Many FLAC and MP3 files are mastered at 48 kHz. Without SRC, these files play at incorrect pitch.
- **Approach:** 147:160 polyphase filter with fixed-point arithmetic. Working buffer is approximately 512 bytes.
- **CPU impact:** Adds approximately 12-15% Core 1 utilization when active.

### DR-2: Dithered 24-bit to 16-bit Noise-Shaping Downsampler
- **Why:** Truncating 24-bit audio to 16-bit introduces approximately 96 dB quantization noise floor. TPDF dithering and noise shaping raises effective perceived quality.
- **Approach:** Implement a simple Lipshitz NS5 noise-shaper as a two-line FIR on the PCM output before feeding the SBC encoder.
- **CPU impact:** Negligible (two multiplies and an add per sample).

### DR-3: ID3v2.4 Syncsafe Integer Handling Improvements
- **Why:** Some ID3v2.4 encoders write extended headers that a minimal parser skips incorrectly, causing the APIC search to fail.
- **Approach:** Full syncsafe integer decoding for frame size fields.

### DR-4: Nokia Display Scrolling Text DMA Overlay
- **Why:** Scrolling a 16px text line currently requires a CPU memcpy of the shifted pixel row then a full-width DMA push per frame.
- **Approach:** Pre-render the full text line once into a PSRAM buffer (128 + max_char_width pixels wide), then on each frame, push only the 128-pixel window using the Nokia driver's `pushImage()`. This reduces CPU-side cost to a pointer increment per frame.

### DR-5: Nokia Display Brightness PWM Control
- **Why:** The Nokia C1-01 LCD backlight is either fully on or off. Adjustable brightness improves battery life in future portable versions.
- **Approach:** Connect the backlight enable line (currently hard-wired) to a reclaimed GPIO and control via `ledc` PWM peripheral.
- **Note:** Requires physical hardware modification; the pin and control GPIO are not yet confirmed.

---

## Known Hardware Limits and Constraints

### Bluetooth
- Effective A2DP ACL bandwidth ceiling: approximately 800 kbps.
- Negotiated bitpool is always the minimum of what the source offers and what the sink accepts.
- Most consumer Bluetooth headphones and speakers cap at bitpool 53.
- SBC XQ quality improvement is fully realized only with sinks that declare a higher max_bitpool.
- Bluedroid audio tick period: 30 ms. Up to 21 SBC frames are encoded per tick.
- Bluedroid internal SBC packet buffer: 4112 bytes. This is fixed by ESP-IDF.
- 16-bit signed PCM is the only input format accepted by the SBC encoder.

### SDMMC
- Maximum hardware SDMMC clock: 40 MHz (SDMMC_FREQ_HIGHSPEED) via the ESP32 SDMMC host.
- 1-bit mode at 40 MHz: 5 MB/s read throughput.
- SD card standard maximum (1-bit, Class 10): approximately 10 MB/s at the card end; ESP32 host limits to 5 MB/s.
- FATFS long filename support requires LFN heap mode (approximately 256-512 bytes per path operation).

### FATFS
- Long filenames currently disabled (CONFIG_FATFS_LFN_NONE=y). Must be enabled before audio files with standard names can be opened.
- exFAT is disabled. SD cards larger than 32 GB must be formatted as FAT32 using third-party tools.

### Display
- TJpgDec supports only 1/2, 1/4, and 1/8 integer downscale factors. Non-power-of-2 album art sizes require a second nearest-neighbour pass.
- Nokia panel maximum SPI clock: 26 MHz.
- No hardware alpha blending or layer compositing.
- Full-screen repaint at 30 fps consumes approximately 8% of Core 1 at 160 MHz.

### IRAM
- Current IRAM usage: 96.9 KB of 128 KB (75.7%).
- Remaining headroom: 31 KB.
- Nokia driver `IRAM_ATTR` hot paths (bit-packing loops) will consume additional IRAM.
- PSRAM cache workaround (for ESP32 rev 0/1) would consume 20-40 KB IRAM. Chip revision must be confirmed before enabling PSRAM.

### Power
- AMS1117-3.3 regulator on the HW-297 is rated at 1.0 A continuous.
- Estimated peak system draw (BT TX + CPU + SD + display): approximately 550 mA.
- A 5V 2A USB supply is required.
- The high-power camera flash LED (GPIO4) is disconnected in this project. Do not drive it.
