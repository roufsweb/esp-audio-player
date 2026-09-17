---
name: ui-animation-design
description: Guidelines and reference implementations for embedded 128x160 Nokia C1-01 LCD UI design, low-overhead micro-animations, text scrolling, dirty-rectangle rendering, and Web Serial Dashboard aesthetics.
---

# UI Design & Animation Skill

This skill defines the technical standards, color palettes, rendering architecture, and animation techniques for both the **embedded Nokia C1-01 LCD (128x160 RGB565)** and the **Web Serial Dashboard**.

---

## 1. Embedded Display Architecture (Nokia C1-01 128x160)

### Hardware Constraints & Memory Limits
- Resolution: 128 width × 160 height.
- Color Format: RGB565 (16 bits per pixel).
- Full frame size: 128 × 160 × 2 bytes = **40,960 bytes (40 KB)**.
- **Rule**: Never allocate full-screen 40 KB framebuffers in internal SRAM. Use **partial dirty-rectangle updates** or a **strip/line buffer** (e.g. 16 scanlines = 4 KB) in internal SRAM with SPI DMA transfers.

### RGB565 Color Palette Tokens
Use a cohesive dark-mode audiophile palette:
- Background: `#0B0F19` (`0x0863` in RGB565) — deep midnight blue/black.
- Card / Surface: `#161E2E` (`0x10E5` in RGB565).
- Accent Primary (Cyan): `#00F2FE` (`0x077F` in RGB565) — progress bars, active playback icons.
- Accent Secondary (Violet): `#8B5CF6` (`0x8AEF` in RGB565) — headers, badges.
- Success (Emerald): `#10B981` (`0x15D0` in RGB565) — Bluetooth connected status.
- Warning (Amber): `#F59E0B` (`0xFCE1` in RGB565) — paused status, connecting.
- Text Primary: `#FFFFFF` (`0xFFFF` in RGB565).
- Text Muted: `#94A3B8` (`0x9536` in RGB565).

---

## 2. Embedded Animation Techniques

### A. Non-Blocking Marquee Ticker (Long Track Titles)
When a song title exceeds 128 pixels width:
1. Render title to an offscreen strip or calculate character offset.
2. Advance pixel offset by 1 px every 40 ms (25 fps).
3. Pause for 1.5 seconds at beginning and end of string.
4. Only redraw the title bounding box (e.g., `Y: 20 to 36, X: 0 to 128`), never the full screen.

### B. Smooth Progress Bar
- Instead of redrawing the full bar on every second tick:
- Update only the bar fill rectangle (`width = (current_sec / total_sec) * 112`).
- Use rounded capsule ends and 1px glow highlights.

### C. Animated Playback Waves / Spectrum Bars
- Render 4 or 5 vertical bars (width 3px, gap 2px) next to track title.
- Cycle heights via a small precalculated sine lookup table:
  ```c
  static const uint8_t s_wave_heights[8] = { 4, 8, 14, 10, 16, 12, 6, 2 };
  ```
- Advance frame counter on each 50ms display tick when `s_state == AUDIO_STATE_PLAYING`.
- Freeze in flat 2px state when paused.

### D. Dirty Rectangle Optimization
- Maintain `dirty_rect_t { uint8_t x, y, w, h; }`.
- Only push pixel data for areas whose state actually changed (e.g. elapsed time digits, volume overlay, status dot).

---

## 3. Web Serial Dashboard Design Rules

- **Theme**: Sleek dark mode with glassmorphism (`backdrop-filter: blur(16px)`).
- **Typography**: Google Fonts Outfit (headings, labels) + JetBrains Mono (numbers, rates, bitpools).
- **Interactive Scrubber**: Click or drag progress bar to seek instantly.
- **Dynamic Telemetry Badges**:
  - Codec badge: `SBC XQ 452 kbps` (vibrant cyan gradient).
  - Rate badge: `44.1 kHz` / `48.0 kHz`.
  - Buffer health gauge: Live percentage with animated color transition (red < 20%, amber < 50%, green > 50%).
