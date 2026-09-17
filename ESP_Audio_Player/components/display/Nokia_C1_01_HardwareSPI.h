#ifndef NOKIA_C1_01_HARDWARESPI_H
#define NOKIA_C1_01_HARDWARESPI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include <driver/spi_master.h>

// ============================================================
// Nokia C1-01 / ST7735 / SPFD54124B â€” 9-bit Hardware SPI Driver
// Target: ESP32-S2 Mini (320 KB SRAM, 2 MB PSRAM)
//
// Key design decisions:
//  â€¢ 9-bit SPI (1 D/C bit + 8 data bits per frame) packed in software.
//  â€¢ Single DMA buffer: at DMA-bottleneck (31% CPU), double-buffering
//    saves only 0.2% frame time â€” not worth 1,152 B (OPT-S1).
//  â€¢ Combined header+pixel DMA: setWindow header (13 B) is prepended to
//    stripe pixel data so _flushStripe issues ONE queued transaction with
//    zero CPU blocking in the pushLine hot path (OPT-S4).
//  â€¢ Transparent rotation API: pushLine(y, row) produces correct output
//    in all 4 rotations. Driver handles GRAM mapping, batching, pixel order.
//  â€¢ queue_size=1 matches the single _trans field â€” no descriptor overrun.
//
// Optimisation ledger:
//  OPT-1   fillRect single-buffer re-queue + OPT-4 fill dirty flag
//  OPT-3   Column-stripe DMA: landscape pushLine needs 1,024 B, not 40 KB
//  OPT-5   pushLine() API: rotation-aware single-row push, all rotations
//  OPT-6   Portrait batch: 4 rows/setWindow -> 40 calls/frame (was 160)
//  OPT-11  IRAM_ATTR on setWindow / _rawSetWindow / pushLine / _flushStripe
//  OPT-S1  Single DMA buffer (double-buf was 0.2% benefit for 1,152 B cost)
//  OPT-S2  DMA buffer sized by combined stripe need; chunk_lines=2 default
//  OPT-S3  _scratch_buf[13] embedded in class -- no heap alloc, no fragmentation
//  OPT-S4  Combined header+pixel queued DMA in _flushStripe -- zero blocking
//          setWindow calls in the pushLine hot path. Eliminates ~9% CPU/frame.
//  OPT-C   _stripe_buf reduced to 1,024 B: portrait K=4 (512 px) dominates
//          landscape K=2 (320 px). LANDSCAPE_K=2 with queued DMA: negligible FPS cost.
// ============================================================

class Nokia_C1_01_HardwareSPI {
public:
    // spi_host: SPI2_HOST (FSPI) or SPI3_HOST (HSPI)
    Nokia_C1_01_HardwareSPI(spi_host_device_t spi_host,
                             int8_t cs_pin, int8_t sck_pin,
                             int8_t mosi_pin, int8_t rst_pin);
    ~Nokia_C1_01_HardwareSPI();

    // begin()
    //   freq_hz:      SPI clock frequency. Max reliable ~26 MHz for Nokia C1-01.
    //   chunk_lines:  Lower bound for pushPixels chunk depth. Buffer is sized to
    //                 max(chunk_buf, combined_stripe_size), so actual buffer may
    //                 be larger -- e.g., chunk_lines=2 gives 576 B chunk buf but
    //                 the combined portrait stripe buf (13+1152=1165 B) wins.
    //                 OPT-S2: default 2 (low-RAM boards with WiFi/BT).
    //   prefer_psram: DMA buffer goes to PSRAM first, keeping internal SRAM free
    //                 for WiFi+AP stacks. On ESP32-S2, GDMA can access PSRAM.
    bool begin(uint32_t freq_hz = 26000000, uint8_t chunk_lines = 2, bool prefer_psram = false);

    // --- Command / data primitives ---
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t data);

    // --- Window & pixel primitives ---
    void setWindow(uint8_t px0, uint8_t py0, uint8_t px1, uint8_t py1);
    void drawPixel(int16_t x, int16_t y, uint16_t color);

    // --- Bulk pixel push (raw RGB565 little-endian, byte-swap done internally) ---
    void pushPixels(uint16_t* colors, uint32_t len);

    // --- Rectangle fills (OPT-1 / OPT-4) ---
    void fillScreen(uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

    // --- Fast lines ---
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);

    // --- Image blit (source array is NEVER modified) ---
    void pushImage(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* data);

    // --- Transparent rotation API (OPT-5/6/S4) ---
    // THE ROTATION CONTRACT:
    //   for (int y = 0; y < disp.height(); y++) {
    //     for (int x = 0; x < disp.width(); x++) row[x] = pixel(x, y);
    //     disp.pushLine(y, row);
    //   }
    //   disp.flush();
    // This IDENTICAL code produces correct output in ALL 4 rotations.
    // Driver handles GRAM mapping, batching, pixel ordering internally.
    void pushLine(int16_t ly, const uint16_t* src);

    // --- Flush all queued DMA transactions (blocking) ---
    // Also flushes any partial landscape stripe buffer.
    void flush();

    // --- Misc ---
    void invertDisplay(bool i);
    void clear_driver_ram();     // clear full physical ST7735 GRAM (incl. invisible offset area)
    void setRotation(uint8_t m); // 0=portrait, 1=landscape, 2=portrait-flipped, 3=landscape-flipped

    int16_t width()  { return _width;  }
    int16_t height() { return _height; }

    // Physical pixel dimensions of this Nokia C1-01 display
    static const int16_t TFTWIDTH  = 128;
    static const int16_t TFTHEIGHT = 160;

    // OPT-3: Portrait stripe depth (rows per DMA burst)
    static const uint8_t STRIPE_K = 4;

private:
    spi_host_device_t   _spi_host;
    int8_t              _cs, _sck, _mosi, _rst;
    spi_device_handle_t _spi;

    uint8_t  _rotation;
    int16_t  _width, _height;

    // OPT-S1: Single DMA buffer.
    uint8_t* _dma_buf;
    uint32_t _dma_buf_size_bytes;    // total bytes in _dma_buf
    uint32_t _max_pixels_per_chunk;  // cached: (_dma_buf_size_bytes / 9) * 4

    // OPT-S3: scratch buffer embedded in class BSS â€” zero heap cost, zero fragmentation.
    // Used exclusively by _rawSetWindow (polling path). Completely separate from _dma_buf.
    uint8_t  _scratch_buf[13];

    // OPT-S1: Single transaction descriptor (queue_size=1).
    int               _queued_trans;
    spi_transaction_t _trans;

    // OPT-4: precomputed fill register â€” skip re-pack when color unchanged.
    uint16_t _fill_color;
    bool     _fill_buf_valid;

    // OPT-3/5/6: Stripe buffer for pushLine batching.
    // Portrait  (rot 0/2): STRIPE_K=4 rows x TFTWIDTH=128 px  = 512 px = 1,024 B
    // Landscape (rot 1/3): LANDSCAPE_K=2 cols x TFTHEIGHT=160 px = 320 px = 640 B
    // Portrait dominates -> buffer sized to STRIPE_K * TFTWIDTH.
    static const uint8_t LANDSCAPE_K = 2;  // columns per DMA burst in landscape mode
    uint16_t _stripe_buf[STRIPE_K * TFTWIDTH]; // 512 pixels = 1,024 bytes
    uint8_t  _stripe_slot;      // next empty slot (0 .. STRIPE_K-1 or LANDSCAPE_K-1)
    int16_t  _stripe_first_ly;  // logical_y of the first slot in the current stripe

    // --- Bit-packing helpers ---
    void write9Bits(uint8_t is_data, uint8_t byte_val);
    // Standard order: sends in[] bytes as-is
    void pack8BytesTo9Bytes(const uint8_t* in, uint8_t* out);
    // Swapped: swaps byte pairs so raw little-endian uint16_t* can be passed directly.
    // in[LO0,HI0, LO1,HI1, ...] -> wire: HI0,LO0, HI1,LO1, ...
    void pack8BytesTo9BytesSwapped(const uint8_t* in, uint8_t* out);

    // Physical GRAM window â€” no rotation transform applied.
    // x0,y0,x1,y1 are post-offset (Nokia +2 col, +1 row already added by caller).
    void _rawSetWindow(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

    // OPT-S4: pack CASET(x0,x1)+PASET(y0,y1)+RAMWR into dst[0..12] (99 bits, no SPI I/O).
    // Called by _rawSetWindow (-> _scratch_buf) and _flushStripe (-> _dma_buf[0..12]).
    void _packSetWindowHeader(uint8_t* dst, uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1);

    // OPT-S4: pack header+pixels into _dma_buf and queue one combined DMA transaction.
    // Eliminates all blocking setWindow calls from the pushLine hot path.
    void _flushStripe();
};

#endif // NOKIA_C1_01_HARDWARESPI_H

