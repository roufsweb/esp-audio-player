// ============================================================
// Nokia_C1_01_HardwareSPI.cpp
// Bulletproof 9-bit SPI display driver for Nokia C1-01 (ST7735 / SPFD54124B)
// Target: ESP32-S2 (320 KB internal SRAM, 2 MB PSRAM, single-core)
// Display: Software landscape only — hardware MADCTL rejected by physical panel.
//
// Original bug fixes (vs. first version):
//  BUG-1  write9Bits used stack memory as DMA source → now uses dedicated scratch buffer
//  BUG-2  DMA buffers forced MALLOC_CAP_INTERNAL → starved WiFi; now fall back to PSRAM
//  BUG-3  pushPixels packed into buffer BEFORE checking if DMA was done with it (race)
//  BUG-4  queue_size=7 exceeded _trans[2] array bounds → now queue_size=2
//  BUG-5  tail-pixel handler used write9Bits (stack-DMA) → now uses short DMA transaction
//  BUG-6  setWindow issued 11 separate SPI transactions → now packed into one 99-bit burst
//  BUG-7  pushImage swapped source array in-place (unsafe with PROGMEM/const) → removed
//  BUG-8  clear_driver_ram window dimensions off by 1 column+row → corrected
//  BUG-9  drawPixel went through pushPixels tail path → now has a dedicated 18-bit fast path
//  BUG-10 invertDisplay did not flush pending DMA before issuing polling command
//  BUG-11 fillRect tail-pixel path used write9Bits → now uses short DMA transaction
//
// Optimisation / hardening pass (STEP + OPT series):
//  STEP-1  begin(chunk_lines=0) caused infinite loop → guard added (clamp to 1)
//  STEP-2  DMA buffer[0] leaked if buffer[1] alloc failed → cleanup before return false
//  STEP-3  heap_caps_check_integrity_addr() misused as location probe → esp_ptr_external_ram()
//  STEP-4  Destructor missing spi_bus_free() → peripheral left active after object destruction
//  STEP-5  write9Bits/setWindow shared _dma_buf[0] as polling scratch (fragile) →
//          dedicated 16-byte _scratch_buf; polling and DMA now use completely separate buffers
//  STEP-6  pushImage had no boundary clipping → negative coords wrapped in uint8_t setWindow
//  STEP-7  Only buf[0] location was logged → now both buffers logged for split-alloc visibility
//  STEP-8  prefer_psram flag added to begin() → DMA buffers go to PSRAM first when WiFi active,
//          keeping all internal SRAM free for WiFi + AP stacks
//  OPT-1   fillRect/fillScreen: single DMA buffer re-queued (no alternation needed for fills)
//  OPT-3   _rawSetWindow + _flushStripe: column-stripe DMA for landscape without framebuffer
//  OPT-4   _fill_buf_valid dirty flag: skip re-pack when same color is reused
//  OPT-5   pushLine(): rotation-aware single-row push, handles all 4 rotation angles
// ============================================================


#include "Nokia_C1_01_HardwareSPI.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"

static inline void delay_ms(uint32_t ms) {
    if (ms < 10) {
        esp_rom_delay_us(ms * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}


// ============================================================
// Constructor / Destructor
// ============================================================

Nokia_C1_01_HardwareSPI::Nokia_C1_01_HardwareSPI(
    spi_host_device_t spi_host,
    int8_t cs_pin, int8_t sck_pin, int8_t mosi_pin, int8_t rst_pin)
    : _spi_host(spi_host),
      _cs(cs_pin), _sck(sck_pin), _mosi(mosi_pin), _rst(rst_pin),
      _spi(nullptr)
{
    _width           = TFTWIDTH;
    _height          = TFTHEIGHT;
    _rotation        = 0;
    _dma_buf         = nullptr;  // OPT-S1: single buffer
    _queued_trans    = 0;
    // OPT-4
    _fill_color      = 0xFFFF;
    _fill_buf_valid  = false;
    // OPT-3/5/6
    _stripe_slot     = 0;
    _stripe_first_ly = 0;
}

Nokia_C1_01_HardwareSPI::~Nokia_C1_01_HardwareSPI() {
    if (_spi) {
        spi_bus_remove_device(_spi);
        spi_bus_free(_spi_host); // STEP-4
    }
    if (_dma_buf) heap_caps_free(_dma_buf); // OPT-S1: single free
    // OPT-S3: _scratch_buf is embedded array — no heap free needed
}

// ============================================================
// begin()
// ============================================================

bool Nokia_C1_01_HardwareSPI::begin(uint32_t freq_hz, uint8_t chunk_lines, bool prefer_psram) {
    // STEP-1: guard against chunk_lines=0
    if (chunk_lines == 0) chunk_lines = 1;
    // OPT-4
    _fill_buf_valid  = false;
    // OPT-3/5/6
    _stripe_slot     = 0;
    _stripe_first_ly = 0;

    // --- OPT-S2/S4: DMA buffer sizing ---
    // Size = max(chunk_buf, combined_stripe_buf) where:
    //   chunk_buf          = chunk_lines x TFTWIDTH pixels packed (for pushPixels)
    //   combined_stripe_buf= 13 (setWindow header) + max_stripe_pixels packed (OPT-S4)
    //
    // Portrait stripe: STRIPE_K x TFTWIDTH = 4 x 128 = 512 px -> 1152 packed bytes
    // Landscape stripe: LANDSCAPE_K x TFTHEIGHT = 2 x 160 = 320 px -> 720 packed bytes
    // Max stripe packed: 1152 bytes (portrait).
    // Combined: 13 + 1152 = 1165 bytes.
    //
    // With chunk_lines=2: chunk_buf = 2 x 128 = 256 px -> 576 packed bytes.
    // 1165 > 576 -> combined_stripe_buf wins -> _dma_buf_size_bytes = 1165.

    uint32_t portrait_stripe_px   = (uint32_t)STRIPE_K   * TFTWIDTH;  // 512 px
    uint32_t landscape_stripe_px  = (uint32_t)LANDSCAPE_K * TFTHEIGHT; // 320 px
    uint32_t max_stripe_px        = (portrait_stripe_px > landscape_stripe_px)
                                    ? portrait_stripe_px : landscape_stripe_px;
    uint32_t max_stripe_packed    = (max_stripe_px * 2 / 8) * 9; // 1152 bytes
    uint32_t combined_stripe_size = 13 + max_stripe_packed;       // 1165 bytes

    uint32_t chunk_px     = (uint32_t)TFTWIDTH * chunk_lines;
    uint32_t chunk_packed = (chunk_px * 2 / 8) * 9;
    if ((chunk_px * 2) % 8) chunk_packed += 9;

    _dma_buf_size_bytes   = (combined_stripe_size > chunk_packed)
                            ? combined_stripe_size : chunk_packed;
    // pushPixels uses the region after the 13-byte header slot for safety,
    // but actually it overwrites from byte 0 (no concurrent use with _flushStripe).
    // _max_pixels_per_chunk computed from full buffer (generous, fills faster).
    _max_pixels_per_chunk = (_dma_buf_size_bytes / 9) * 4;

    // --- OPT-S1: Single DMA buffer allocation ---
    uint32_t primary_caps  = prefer_psram
        ? (MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM)
        : (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint32_t fallback_caps = prefer_psram
        ? (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
        : (MALLOC_CAP_DMA);

    _dma_buf = (uint8_t*)heap_caps_malloc(_dma_buf_size_bytes, primary_caps);
    if (!_dma_buf)
        _dma_buf = (uint8_t*)heap_caps_malloc(_dma_buf_size_bytes, fallback_caps);
    if (!_dma_buf) {
        printf("[NokiaDrv] FATAL: DMA buffer alloc failed (%lu bytes)\n",
               (unsigned long)_dma_buf_size_bytes);
        return false;
    }

    const char* loc = esp_ptr_external_ram(_dma_buf) ? "PSRAM" : "SRAM";
    printf("[NokiaDrv] buf=%lu B@%s | scratch=13B@BSS | SRAM free: %lu B\n",
           (unsigned long)_dma_buf_size_bytes, loc,
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // OPT-S3: _scratch_buf is uint8_t[13] embedded in class -- no heap alloc needed.

    // --- SPI bus ---
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num     = _sck;
    buscfg.mosi_io_num     = _mosi;
    buscfg.miso_io_num     = -1;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = _dma_buf_size_bytes; // combined buf already has headroom

    esp_err_t ret = spi_bus_initialize(_spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        printf("[NokiaDrv] SPI bus init failed: %s\n", esp_err_to_name(ret));
        return false;
    }

    // --- SPI device ---
    // OPT-S1: queue_size=1 -- single buffer, max 1 in-flight transaction.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = freq_hz;
    devcfg.mode           = 0;
    devcfg.spics_io_num   = _cs;
    devcfg.queue_size     = 1;  // OPT-S1: was 2; single buffer needs only 1 slot
    devcfg.command_bits   = 0;
    devcfg.address_bits   = 0;
    devcfg.dummy_bits     = 0;

    ret = spi_bus_add_device(_spi_host, &devcfg, &_spi);
    if (ret != ESP_OK) {
        printf("[NokiaDrv] SPI device add failed: %s\n", esp_err_to_name(ret));
        return false;
    }

    // --- Hardware reset ---
    if (_rst >= 0) {
        gpio_set_direction((gpio_num_t)_rst, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)_rst, 1); delay_ms(50);
        gpio_set_level((gpio_num_t)_rst, 0); delay_ms(50);
        gpio_set_level((gpio_num_t)_rst, 1); delay_ms(150);
    }

    // --- Display init sequence ---
    sendCommand(0x01); delay_ms(150); // SWRESET
    sendCommand(0x11); delay_ms(150); // SLPOUT
    sendCommand(0x3A); sendData(0x05); // COLMOD: 16-bit RGB565
    sendCommand(0x36); sendData(0x00); // MADCTL: no mirror/flip

    // Clear full driver RAM BEFORE DISPON -> no white flash or garbage on boot.
    clear_driver_ram();

    sendCommand(0x29); delay_ms(10); // DISPON
    return true;
}

// ============================================================
// Low-level 9-bit write (command or data byte)
// ============================================================

// OPT-S3: _scratch_buf is now uint8_t[13] embedded in the class (no heap alloc).
// write9Bits uses [0..1]; _rawSetWindow uses [0..12]. Both are safe since:
//  - single-threaded (no concurrent access)
//  - embedded in class BSS (always DMA-safe on ESP32 internal SRAM)
void IRAM_ATTR Nokia_C1_01_HardwareSPI::write9Bits(uint8_t is_data, uint8_t byte_val) {
    // Drain queued DMA before issuing polling transfer — cannot interleave.
    while (_queued_trans > 0) {
        spi_transaction_t* done;
        spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
        --_queued_trans;
    }
    // One 9-bit frame: [D/C=is_data][B7..B0], MSB-first, top-aligned in 2 bytes.
    _scratch_buf[0] = (is_data ? 0x80 : 0x00) | (byte_val >> 1);
    _scratch_buf[1] = (byte_val & 0x01) << 7;
    spi_transaction_t t = {};
    t.length    = 9;
    t.tx_buffer = _scratch_buf;
    spi_device_polling_transmit(_spi, &t);
}

void IRAM_ATTR Nokia_C1_01_HardwareSPI::sendCommand(uint8_t cmd)  { write9Bits(0, cmd);  }
void IRAM_ATTR Nokia_C1_01_HardwareSPI::sendData   (uint8_t data) { write9Bits(1, data); }

// ============================================================
// flush() — drain partial stripe buffer + all queued DMA transactions
// ============================================================
// This is the frame-boundary call. After pushing all rows with pushLine(),
// call flush() to:
//   1. Send any partially-filled stripe buffer (< STRIPE_K rows accumulated).
//   2. Block until all queued DMA transactions complete.
//   3. Reset _stripe_first_ly to 0 for the next frame.
//
// OPT-6: portrait batching accumulates STRIPE_K rows per setWindow call.
// Without flush(), the last partial batch (if height % STRIPE_K != 0) would
// be silently dropped. flush() ensures every row is sent.

void Nokia_C1_01_HardwareSPI::flush() {
    // OPT-6: drain any partial stripe before draining DMA
    if (_stripe_slot > 0) {
        _flushStripe();
        _stripe_slot = 0;
    }
    // Drain DMA queue
    while (_queued_trans > 0) {
        spi_transaction_t* done;
        spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
        --_queued_trans;
    }
    // Reset frame-level stripe state so next frame starts at row 0
    _stripe_first_ly = 0;
}

// ============================================================
// Bit-packing helpers
// ============================================================

// Standard packer: 8 raw bytes → 9 packed 9-bit-frame bytes.
// Each output 9-bit frame = [D/C=1][B7..B0] of one input byte.
// The 8 D/C bits (all=1 for pixel data) are woven in between the data bits.
// Caller MUST provide bytes already in big-endian (HI, LO) order for RGB565.
inline void IRAM_ATTR Nokia_C1_01_HardwareSPI::pack8BytesTo9Bytes(
    const uint8_t* in, uint8_t* out)
{
    out[0] =  (1 << 7)        | (in[0] >> 1);
    out[1] = ((in[0] & 0x01) << 7) | (1 << 6) | (in[1] >> 2);
    out[2] = ((in[1] & 0x03) << 6) | (1 << 5) | (in[2] >> 3);
    out[3] = ((in[2] & 0x07) << 5) | (1 << 4) | (in[3] >> 4);
    out[4] = ((in[3] & 0x0F) << 4) | (1 << 3) | (in[4] >> 5);
    out[5] = ((in[4] & 0x1F) << 3) | (1 << 2) | (in[5] >> 6);
    out[6] = ((in[5] & 0x3F) << 2) | (1 << 1) | (in[6] >> 7);
    out[7] = ((in[6] & 0x7F) << 1) |  1;
    out[8] =   in[7];
}

// Swapped packer (BUG-7 FIX): identical to above but swaps byte pairs inline.
// Accepts raw little-endian uint16_t pixel data (as cast to uint8_t*):
//   in[] = [LO0, HI0, LO1, HI1, LO2, HI2, LO3, HI3]
// Wire order produced:
//   HI0, LO0, HI1, LO1, HI2, LO2, HI3, LO3  (correct RGB565 big-endian for ST7735)
// This means pushPixels() and pushImage() NEVER need to mutate the source buffer.
inline void IRAM_ATTR Nokia_C1_01_HardwareSPI::pack8BytesTo9BytesSwapped(
    const uint8_t* in, uint8_t* out)
{
    //            Pixel 0: HI=in[1], LO=in[0]  |  Pixel 1: HI=in[3], LO=in[2]
    //            Pixel 2: HI=in[5], LO=in[4]  |  Pixel 3: HI=in[7], LO=in[6]
    out[0] =  (1 << 7)        | (in[1] >> 1);
    out[1] = ((in[1] & 0x01) << 7) | (1 << 6) | (in[0] >> 2);
    out[2] = ((in[0] & 0x03) << 6) | (1 << 5) | (in[3] >> 3);
    out[3] = ((in[3] & 0x07) << 5) | (1 << 4) | (in[2] >> 4);
    out[4] = ((in[2] & 0x0F) << 4) | (1 << 3) | (in[5] >> 5);
    out[5] = ((in[5] & 0x1F) << 3) | (1 << 2) | (in[4] >> 6);
    out[6] = ((in[4] & 0x3F) << 2) | (1 << 1) | (in[7] >> 7);
    out[7] = ((in[7] & 0x7F) << 1) |  1;
    out[8] =   in[6];
}

// ============================================================
// setWindow — BUG-6 FIX: one 99-bit DMA burst for all 11 bytes
// ============================================================
//
// Previous: 11 individual sendCommand/sendData calls → 11 SPI transactions,
//           11 CS toggles, 11 stack-DMA accesses (BUG-1).
// Fixed: pre-pack the entire CASET + PASET + RAMWR sequence into 13 bytes
//        (11 × 9 bits = 99 bits) and fire as a single polling transaction.

// OPT-11: IRAM_ATTR eliminates i-cache miss stalls when setWindow is called
// 160 times per portrait frame. At 32 KB IRAM budget this costs ~500 bytes.
void IRAM_ATTR Nokia_C1_01_HardwareSPI::setWindow(
    uint8_t px0, uint8_t py0, uint8_t px1, uint8_t py1)
{
    // Flush DMA before issuing a polling transfer (cannot interleave)
    while (_queued_trans > 0) {
        spi_transaction_t* done;
        spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
        --_queued_trans;
    }

    uint8_t x0, x1, y0, y1;
    switch (_rotation) {
        case 1:
            x0 = TFTWIDTH  - 1 - py1; y0 = px0;
            x1 = TFTWIDTH  - 1 - py0; y1 = px1;
            break;
        case 2:
            x0 = TFTWIDTH  - 1 - px1; y0 = TFTHEIGHT - 1 - py1;
            x1 = TFTWIDTH  - 1 - px0; y1 = TFTHEIGHT - 1 - py0;
            break;
        case 3:
            x0 = py0;                 y0 = TFTHEIGHT - 1 - px1;
            x1 = py1;                 y1 = TFTHEIGHT - 1 - px0;
            break;
        default: // case 0
            x0 = px0;                 y0 = py0;
            x1 = px1;                 y1 = py1;
            break;
    }
    
    // Ensure min <= max after rotation
    if (x0 > x1) { uint8_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { uint8_t t = y0; y0 = y1; y1 = t; }

    // Physical offset for Nokia C1-01
    x0 += 2; x1 += 2;
    y0 += 1; y1 += 1;

    // Pack: CASET(0x2A) 0x00 x0 0x00 x1  PASET(0x2B) 0x00 y0 0x00 y1  RAMWR(0x2C)
    // D/C:    0          1   1   1   1     0           1   1   1   1     0
    // STEP-5: use _scratch_buf (16 B, always SRAM, dedicated for polling transactions)
    uint8_t* tx = _scratch_buf;

    tx[ 0] = (0 << 7)        | (0x2A >> 1);
    tx[ 1] = ((0x2A & 1) << 7) | (1 << 6) | (0x00 >> 2);
    tx[ 2] = ((0x00 & 3) << 6) | (1 << 5) | (x0  >> 3);
    tx[ 3] = ((x0  & 7) << 5) | (1 << 4) | (0x00 >> 4);
    tx[ 4] = ((0x00 & 0xF) << 4) | (1 << 3) | (x1  >> 5);
    tx[ 5] = ((x1  & 0x1F) << 3) | (0 << 2) | (0x2B >> 6);
    tx[ 6] = ((0x2B & 0x3F) << 2) | (1 << 1) | (0x00 >> 7);
    tx[ 7] = ((0x00 & 0x7F) << 1) |  1;
    tx[ 8] = y0;
    tx[ 9] = (1 << 7)        | (0x00 >> 1);
    tx[10] = ((0x00 & 1) << 7) | (1 << 6) | (y1  >> 2);
    tx[11] = ((y1  & 3) << 6) | (0 << 5) | (0x2C >> 3);
    tx[12] = ((0x2C & 7) << 5);

    spi_transaction_t t = {};
    t.length    = 99; // 11 × 9 bits
    t.tx_buffer = tx;
    spi_device_polling_transmit(_spi, &t);
}

// ============================================================
// setRotation
// ============================================================

void Nokia_C1_01_HardwareSPI::setRotation(uint8_t m) {
    flush(); // drain any pending DMA before geometry changes
    _rotation = m % 4;
    switch (_rotation) {
        case 0: case 2: _width = TFTWIDTH;  _height = TFTHEIGHT; break;
        case 1: case 3: _width = TFTHEIGHT; _height = TFTWIDTH;  break;
    }
    // OPT-3/5: reset stripe buffer — partial stripes from old rotation are invalid
    _stripe_slot     = 0;
    _stripe_first_ly = 0;
    // OPT-4: invalidate fill cache — buffer geometry may have changed
    _fill_buf_valid  = false;
}

// ============================================================
// drawPixel — BUG-9 FIX: dedicated 18-bit fast path
// ============================================================

void IRAM_ATTR Nokia_C1_01_HardwareSPI::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= _width || y < 0 || y >= _height) return;
    setWindow(x, y, x, y);
    // Pack one RGB565 pixel as two 9-bit frames = 18 bits = 3 bytes.
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    uint8_t* tx = _dma_buf; // OPT-S1: single buffer
    tx[0] = (1 << 7) | (hi >> 1);
    tx[1] = ((hi & 1) << 7) | (1 << 6) | (lo >> 2);
    tx[2] = ((lo & 3) << 6);
    spi_transaction_t t = {};
    t.length    = 18;
    t.tx_buffer = tx;
    spi_device_polling_transmit(_spi, &t);
}

// ============================================================
// fillScreen / fillRect
// ============================================================

void Nokia_C1_01_HardwareSPI::fillScreen(uint16_t color) {
    fillRect(0, 0, _width, _height, color);
}

void Nokia_C1_01_HardwareSPI::fillRect(
    int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    // --- Clip to screen ---
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= _width || y >= _height || w <= 0 || h <= 0) return;
    if (x + w > _width)  w = _width  - x;
    if (y + h > _height) h = _height - y;

    setWindow(x, y, x + w - 1, y + h - 1);

    // OPT-4: only re-pack and re-fill buf[0] if color changed since last fill.
    // _fill_buf_valid is cleared by pushPixels (which overwrites buf[0]) and begin().
    if (!_fill_buf_valid || _fill_color != color) {
        _fill_color = color;
        _fill_buf_valid = false; // mark invalid until buf actually packed

        uint8_t hi = color >> 8;
        uint8_t lo = color & 0xFF;
        uint8_t cbuf[8] = { hi, lo, hi, lo, hi, lo, hi, lo };
        uint8_t packed_9[9];
        pack8BytesTo9Bytes(cbuf, packed_9);

        // OPT-1: pre-fill buf[0] only (fill path never uses buf[1]).
        uint32_t chunks = _dma_buf_size_bytes / 9;
        for (uint32_t i = 0; i < chunks; ++i)
            memcpy(_dma_buf + i * 9, packed_9, 9);

        _fill_buf_valid = true;
    }

    uint32_t pixels_per_buf = _max_pixels_per_chunk;
    uint32_t pixels_left    = (uint32_t)w * (uint32_t)h;

    while (pixels_left > 0) {
        uint32_t p = pixels_left;
        if (p > pixels_per_buf) p = pixels_per_buf;
        else                    p = (p / 4) * 4;

        // OPT-S1: single buffer -- wait for it to be free before re-queueing.
        if (_queued_trans > 0) {
            spi_transaction_t* done;
            spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
            --_queued_trans;
        }
        if (p > 0) {
            _trans           = {};
            _trans.length    = (p / 4) * 9 * 8;
            _trans.tx_buffer = _dma_buf;
            spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
            _queued_trans = 1;
            pixels_left -= p;
        } else {
            // Tail: 1-3 pixels. Pattern at start of buf is correct.
            if (pixels_left > 0) {
                _trans           = {};
                _trans.length    = pixels_left * 18;
                _trans.tx_buffer = _dma_buf;
                spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
                _queued_trans = 1;
                pixels_left = 0;
            }
        }
    }
}

// ============================================================
// drawFastVLine / drawFastHLine
// ============================================================

void Nokia_C1_01_HardwareSPI::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    fillRect(x, y, 1, h, color);
}

void Nokia_C1_01_HardwareSPI::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    fillRect(x, y, w, 1, color);
}

// ============================================================
// pushPixels — BUG-3 FIX (race condition) + BUG-5 FIX (tail pixels)
// ============================================================
//
// Accepts raw little-endian uint16_t* pixel data (no pre-swap needed).
// pack8BytesTo9BytesSwapped handles the big-endian conversion inline.
//
// Critical ordering: wait for the DMA buffer to be free BEFORE packing into it,
// not after. Packing into an in-flight DMA buffer corrupts the SPI stream.

// OPT-S1: Single-buffer pushPixels.
// BUG-3 FIX preserved: wait for buffer to be free BEFORE packing into it.
void IRAM_ATTR Nokia_C1_01_HardwareSPI::pushPixels(uint16_t* colors, uint32_t len) {
    if (!colors || len == 0) return;
    _fill_buf_valid = false; // OPT-4: buf will be overwritten

    const uint8_t* src         = (const uint8_t*)colors;
    uint32_t       max_per_buf = _max_pixels_per_chunk;
    uint32_t       pixels_done = 0;

    while (pixels_done < len) {
        uint32_t remaining = len - pixels_done;
        uint32_t chunk     = (remaining >= max_per_buf)
                             ? max_per_buf
                             : (remaining / 4) * 4;

        // OPT-S1: wait for single buffer to be free before packing.
        if (_queued_trans > 0) {
            spi_transaction_t* done;
            spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
            --_queued_trans;
        }

        if (chunk > 0) {
            uint32_t in_off  = pixels_done * 2;
            uint32_t out_off = 0;
            uint32_t to_pack = chunk * 2;
            while (to_pack >= 8) {
                pack8BytesTo9BytesSwapped(src + in_off, _dma_buf + out_off);
                in_off  += 8; out_off += 9; to_pack -= 8;
            }
            _trans           = {};
            _trans.length    = out_off * 8;
            _trans.tx_buffer = _dma_buf;
            spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
            ++_queued_trans;
            pixels_done += chunk;
        } else {
            // BUG-5 FIX: 1-3 tail pixels. Zero-pad to full 8-byte group.
            uint32_t tail   = remaining;
            uint8_t  tmp[8] = {0};
            uint32_t in_off = pixels_done * 2;
            for (uint32_t i = 0; i < tail * 2; ++i) tmp[i] = src[in_off + i];
            pack8BytesTo9BytesSwapped(tmp, _dma_buf);
            _trans           = {};
            _trans.length    = tail * 18;
            _trans.tx_buffer = _dma_buf;
            spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
            ++_queued_trans;
            pixels_done += tail;
        }
    }
}

// ============================================================
// pushImage — BUG-7 FIX: source array is NEVER modified
// ============================================================

void Nokia_C1_01_HardwareSPI::pushImage(
    int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* data)
{
    // STEP-6: full boundary guard (was only checking fully-off-screen case).
    // Without this, negative x/y values cast to uint8_t in setWindow and wrap
    // around to wrong GRAM addresses, writing pixels to garbage locations.
    if (w <= 0 || h <= 0) return;
    if (x >= _width  || y >= _height)  return;
    if (x + w <= 0   || y + h <= 0)    return;
    // Clamp right/bottom edges to screen bounds
    if (x + w > _width)  w = _width  - x;
    if (y + h > _height) h = _height - y;
    // Note: left/top clipping (x<0 or y<0) requires a stride parameter to skip
    // source rows correctly — reject those cases to prevent corrupt output.
    if (x < 0 || y < 0) return;

    flush();
    setWindow((uint8_t)x, (uint8_t)y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1));
    // BUG-7 FIX: pushPixels handles byte-swapping inline via pack8BytesTo9BytesSwapped.
    // The 'data' pointer is passed through and never written to.
    pushPixels(data, (uint32_t)w * (uint32_t)h);
}

// ============================================================
// invertDisplay — BUG-10 FIX: flush before polling command
// ============================================================

void Nokia_C1_01_HardwareSPI::invertDisplay(bool i) {
    flush(); // BUG-10 FIX: drain DMA before issuing polling command
    sendCommand(i ? 0x21 : 0x20); // INVON : INVOFF
}

// ============================================================
// clear_driver_ram — BUG-8 FIX: correct physical ST7735 window
// ============================================================
//
// Previous version set X-end to 128+2=130 (0x82) and Y-end to 160+1=161 (0xA1),
// which is 1 pixel too wide and 1 pixel too tall, causing a wrap-around write
// that left a 1-pixel ghost artifact on first boot.
//
// Correct physical window:
//   X: 0 to 129 (= 127+2, last visible column incl. +2 offset) → 130 columns
//   Y: 0 to 160 (= 159+1, last visible row   incl. +1 offset) → 161 rows
//   Total: 130 × 161 = 20930 pixels (covers full GRAM incl. invisible offset area)

void Nokia_C1_01_HardwareSPI::clear_driver_ram() {
    flush();

    // Raw CASET/PASET: bypass logical setWindow to address full physical GRAM.
    sendCommand(0x2A);
    sendData(0x00); sendData(0x00); // X start = 0
    sendData(0x00); sendData(0x81); // X end   = 129 (BUG-8 FIX)
    sendCommand(0x2B);
    sendData(0x00); sendData(0x00); // Y start = 0
    sendData(0x00); sendData(0xA0); // Y end   = 160 (BUG-8 FIX)
    sendCommand(0x2C);              // RAM write start

    // Build zero-fill pattern (all black)
    uint8_t zeros[8] = {0};
    uint8_t packed_9[9];
    pack8BytesTo9Bytes(zeros, packed_9);
    uint32_t chunks = _dma_buf_size_bytes / 9;
    for (uint32_t i = 0; i < chunks; ++i)
        memcpy(_dma_buf + i * 9, packed_9, 9); // OPT-S1: single buffer

    // OPT-S1: single-buffer re-queue (same as fillRect OPT-1 pattern).
    uint32_t pixels_per_buf = chunks * 4;
    uint32_t pixels_left    = 130UL * 161UL; // BUG-8 FIX: correct GRAM size

    while (pixels_left > 0) {
        uint32_t p = pixels_left;
        if (p > pixels_per_buf) p = pixels_per_buf;
        else                    p = (p / 4) * 4;

        if (_queued_trans > 0) {
            spi_transaction_t* done;
            spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
            --_queued_trans;
        }
        if (p > 0) {
            _trans           = {};
            _trans.length    = (p / 4) * 9 * 8;
            _trans.tx_buffer = _dma_buf;
            spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
            ++_queued_trans;
            pixels_left -= p;
        } else {
            // tail 1-3 pixels — buf already holds the pattern at the start
            if (pixels_left > 0) {
                _trans           = {};
                _trans.length    = pixels_left * 18;
                _trans.tx_buffer = _dma_buf;
                spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
                ++_queued_trans;
                pixels_left = 0;
            }
        }
    }

    flush(); // Block until clear is complete
}

// ============================================================
// OPT-3: _rawSetWindow — physical GRAM window, no rotation transform
// ============================================================
// x0,y0,x1,y1 are post-offset coordinates (Nokia +2 col / +1 row already added).

// OPT-11: IRAM_ATTR — called from _flushStripe() which is on the hot pushLine path.
void IRAM_ATTR Nokia_C1_01_HardwareSPI::_rawSetWindow(
    uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    while (_queued_trans > 0) {
        spi_transaction_t* done;
        spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
        --_queued_trans;
    }
    _packSetWindowHeader(_scratch_buf, x0, y0, x1, y1);
    spi_transaction_t t = {};
    t.length    = 99;
    t.tx_buffer = _scratch_buf;
    spi_device_polling_transmit(_spi, &t);
}

// ============================================================
// OPT-S4: _packSetWindowHeader -- pure bit-packing, no SPI call
// ============================================================
// Packs CASET(x0,x1) + PASET(y0,y1) + RAMWR into dst[0..12] as
// 11 x 9-bit frames (99 bits). Called by both:
//   _rawSetWindow  -> into _scratch_buf, sent by polling_transmit
//   _flushStripe   -> into _dma_buf[0..12], combined with pixel DMA

void Nokia_C1_01_HardwareSPI::_packSetWindowHeader(
    uint8_t* tx, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    tx[ 0] = (0 << 7)             | (0x2A >> 1);
    tx[ 1] = ((0x2A & 1) << 7)   | (1 << 6) | (0x00 >> 2);
    tx[ 2] = ((0x00 & 3) << 6)   | (1 << 5) | (x0  >> 3);
    tx[ 3] = ((x0  & 7) << 5)    | (1 << 4) | (0x00 >> 4);
    tx[ 4] = ((0x00 & 0xF) << 4) | (1 << 3) | (x1  >> 5);
    tx[ 5] = ((x1 & 0x1F) << 3)  | (0 << 2) | (0x2B >> 6);
    tx[ 6] = ((0x2B & 0x3F) << 2)| (1 << 1) | (0x00 >> 7);
    tx[ 7] = ((0x00 & 0x7F) << 1)| 1;
    tx[ 8] = y0;
    tx[ 9] = (1 << 7)             | (0x00 >> 1);
    tx[10] = ((0x00 & 1) << 7)   | (1 << 6) | (y1  >> 2);
    tx[11] = ((y1  & 3) << 6)    | (0 << 5) | (0x2C >> 3);
    tx[12] = ((0x2C & 7) << 5);
}

// ============================================================
// OPT-S4: _flushStripe -- combined header+pixel queued DMA
// ============================================================
//
// Instead of _rawSetWindow (polling, ~50 us CPU block) + pushPixels (queued DMA),
// this packs the 13-byte window header into _dma_buf[0..12], pixel data into
// _dma_buf[13..], and queues ONE combined DMA transaction. Zero blocking SPI
// calls in the pushLine hot path -> ~9% CPU freed per frame.
//
// Portrait 0:   STRIPE_K=4 rows, physical y = [first_ly+1 .. first_ly+slot]
// Portrait 2:   same, rows reversed, partial offset = (K-slot)*TFTWIDTH in buf
// Landscape 1:  LANDSCAPE_K=2 columns, physical x = [129-first_ly-slot+1 .. 129-first_ly]
// Landscape 3:  LANDSCAPE_K=2 columns, physical x = [first_ly+2 .. first_ly+slot+1]

void IRAM_ATTR Nokia_C1_01_HardwareSPI::_flushStripe() {
    if (_stripe_slot == 0) return;

    // Drain any in-flight transaction before writing into _dma_buf.
    while (_queued_trans > 0) {
        spi_transaction_t* done;
        spi_device_get_trans_result(_spi, &done, portMAX_DELAY);
        --_queued_trans;
    }

    // Pack pixels from src into _dma_buf[13..] and queue a combined transaction.
    auto sendCombined = [this](uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1,
                               const uint16_t* src, uint32_t npixels) {
        _packSetWindowHeader(_dma_buf, x0, y0, x1, y1);
        uint8_t*       dst      = _dma_buf + 13;
        const uint8_t* srcp     = (const uint8_t*)src;
        uint32_t       raw_left = npixels * 2;
        while (raw_left >= 8) {
            pack8BytesTo9BytesSwapped(srcp, dst);
            srcp += 8; dst += 9; raw_left -= 8;
        }
        _trans           = {};
        _trans.length    = 99 + npixels * 18;
        _trans.tx_buffer = _dma_buf;
        spi_device_queue_trans(_spi, &_trans, portMAX_DELAY);
        _queued_trans = 1;
    };

    if (_rotation == 0) {
        uint8_t y0 = (uint8_t)(_stripe_first_ly + 1);
        uint8_t y1 = (uint8_t)(_stripe_first_ly + _stripe_slot);
        sendCombined(2, y0, 129, y1, _stripe_buf, (uint32_t)_stripe_slot * TFTWIDTH);

    } else if (_rotation == 2) {
        uint32_t start_idx = (uint32_t)(STRIPE_K - _stripe_slot) * TFTWIDTH;
        uint8_t  y0 = (uint8_t)(160 - _stripe_first_ly - _stripe_slot + 1);
        uint8_t  y1 = (uint8_t)(160 - _stripe_first_ly);
        sendCombined(2, y0, 129, y1, _stripe_buf + start_idx, (uint32_t)_stripe_slot * TFTWIDTH);

    } else if (_rotation == 1) {
        uint8_t x0 = (uint8_t)((127 - (_stripe_first_ly + _stripe_slot - 1)) + 2);
        uint8_t x1 = (uint8_t)((127 - _stripe_first_ly) + 2);
        sendCombined(x0, 1, x1, (uint8_t)TFTHEIGHT, _stripe_buf, (uint32_t)_stripe_slot * TFTHEIGHT);

    } else {
        uint8_t x0 = (uint8_t)(_stripe_first_ly + 2);
        uint8_t x1 = (uint8_t)(_stripe_first_ly + _stripe_slot - 1 + 2);
        sendCombined(x0, 1, x1, (uint8_t)TFTHEIGHT, _stripe_buf, (uint32_t)_stripe_slot * TFTHEIGHT);
    }
}

// ============================================================
// OPT-5/6/S4: pushLine -- fully transparent rotation API
// ============================================================

void IRAM_ATTR Nokia_C1_01_HardwareSPI::pushLine(int16_t ly, const uint16_t* src) {

    if (_rotation == 0) {
        // OPT-6+S4: accumulate STRIPE_K portrait rows, push as one combined DMA burst.
        uint8_t slot = _stripe_slot;
        memcpy(&_stripe_buf[(uint32_t)slot * TFTWIDTH], src, (uint32_t)TFTWIDTH * 2);
        if (++_stripe_slot == STRIPE_K) {
            _flushStripe();
            _stripe_slot     = 0;
            _stripe_first_ly += STRIPE_K;
        }

    } else if (_rotation == 2) {
        // OPT-6+S4: portrait 180 — reversed rows stored in reverse buffer order.
        uint8_t   slot = _stripe_slot;
        uint16_t* dst  = &_stripe_buf[(uint32_t)(STRIPE_K - 1 - slot) * TFTWIDTH];
        for (int16_t i = 0; i < TFTWIDTH; ++i) dst[i] = src[TFTWIDTH - 1 - i];
        if (++_stripe_slot == STRIPE_K) {
            _flushStripe();
            _stripe_slot     = 0;
            _stripe_first_ly += STRIPE_K;
        }

    } else if (_rotation == 1) {
        // OPT-3+S4: landscape 90 CW — LANDSCAPE_K=2 column stripe transposition.
        // Inverse map: logical_x = raw_row, logical_y = 127 - raw_col.
        // Store slot reversed so GRAM left-to-right fills physical columns correctly.
        uint8_t slot = _stripe_slot;
        for (int16_t lx = 0; lx < TFTHEIGHT; ++lx)
            _stripe_buf[(uint32_t)lx * LANDSCAPE_K + (LANDSCAPE_K - 1 - slot)] = src[lx];
        if (++_stripe_slot == LANDSCAPE_K) {
            _flushStripe();
            _stripe_slot     = 0;
            _stripe_first_ly += LANDSCAPE_K;
        }

    } else { // rotation == 3
        // OPT-3+S4: landscape 270 CCW — LANDSCAPE_K=2 column stripe transposition reversed.
        // Inverse map: logical_x = 159 - raw_row, logical_y = raw_col.
        uint8_t slot = _stripe_slot;
        for (int16_t lx = 0; lx < TFTHEIGHT; ++lx)
            _stripe_buf[(uint32_t)(TFTHEIGHT - 1 - lx) * LANDSCAPE_K + slot] = src[lx];
        if (++_stripe_slot == LANDSCAPE_K) {
            _flushStripe();
            _stripe_slot     = 0;
            _stripe_first_ly += LANDSCAPE_K;
        }
    }
}
