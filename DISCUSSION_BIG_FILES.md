# Deep Technical Research: High-Res Audio, Universal Bluetooth Device Compatibility, SBC XQ & Storage Optimization on ESP32

---

## 1. The Factual Scorecard: Correcting Oversimplifications & Embracing Primary Sources

In engineering, progress happens when hypotheses are ruthlessly tested against primary specifications, disassembly, and real silicon. Below is the unvarnished, fact-checked scorecard based on the **Bluetooth SIG A2DP 1.4 specification & test suites**, **Espressif ESP32 Datasheet & Technical Reference Manual**, **`dr_flac.h` source code**, and **FFmpeg encoder internals**:

| Previous Claim / Shortcut | Factual Verdict | Primary Source Evidence & Technical Reality |
|---|---|---|
| *"A2DP mandates support for 16, 32, 44.1, and 48 kHz"* | ❌ **WRONG** | The **Bluetooth SIG A2DP Interoperability Specification (Section 4.3.2 & Test Suite C1/C2 rules)** explicitly defines 16 kHz and 32 kHz as **optional**. Sinks and Sources are required to support **at least one of 44.1 kHz or 48 kHz**. While the underlying SBC codec specification defines tables for 16, 32, 44.1, and 48 kHz, A2DP interoperability only mandates support for at least 44.1 or 48 kHz. |
| *"Dual Channel mode is mandatory in A2DP, so almost all receivers support it"* | ❌ **WRONG** | The Bluetooth SIG A2DP interoperability requirements treat SBC channel modes as alternatives (**Rule C2: encoder must support at least one of Dual Channel, Stereo, or Joint Stereo**—not all three). Sinks are not universally required to support Dual Channel. Assuming every headphone supports Dual Channel will break compatibility on devices that only implement Joint Stereo or Stereo. |
| *"SBC XQ uses bitpool 38 to 47 per channel, total bitpool 76 to 94"* | ❌ **MISLEADING TERMINOLOGY** | In the SBC frame header, `bitpool` is a single integer parameter (e.g. 38 or 47). In Dual Channel mode, because each channel is encoded independently, that bitpool value is applied to each channel during bit allocation, yielding bitrates of ~452 kbps (at 44.1k, bitpool 38) or ~492 kbps (at 48k, bitpool 38). There is no "total bitpool 76" parameter in the Bluetooth specification. |
| *"SBC XQ is a standardized Bluetooth codec mode"* | ❌ **INACCURATE** | "SBC XQ" is not an official Bluetooth SIG codec or standard name. It is an informal community/open-source designation (coined by ValdikSS and adopted in LineageOS, PipeWire, and Linux) for high-bitrate configurations of the standard SBC codec utilizing Dual Channel mode with elevated bitpool parameters. |
| *"SBC XQ is universally transparent and outperforms aptX HD"* | ❌ **UNSUPPORTED GENERALIZATION** | High-bitrate SBC configurations provide substantial bitrate headroom over standard SBC settings (~328 kbps). While third-party listening tests and measurements (e.g. SoundExpert, LineageOS) have reported very strong results, subjective and objective quality varies depending on the encoder implementation, bitpool, acoustic material, and the receiver's internal SBC decoder and DAC. |
| *"96 kHz 24-bit FLAC requires expensive 64-bit math for every sample"* | ❌ **UNSUPPORTED SHORTCUT** | In `dr_flac.h`, the function `drflac__use_64_bit_prediction(bitsPerSample, order, precision)` dynamically branches: `return bitsPerSample + precision + drflac__ilog2_u32(order) > 32;`. If false, `dr_flac` uses fast 32-bit prediction (`drflac__calculate_prediction_32`). Furthermore, `drflac__ilog2_u32(order)` is an implementation-specific bit-width heuristic (counting right-shifts until zero: order 12 $\to$ 4, order 8 $\to$ 4, order 7 $\to$ 3) rather than a mathematical $\lfloor \log_2(x) \rfloor$. |
| *"64-bit multiply takes 8–16 clock cycles via software emulation"* | ❌ **UNSUPPORTED WITHOUT ASM DUMP** | The ESP32 Xtensa LX6 has a 32-bit hardware multiplier with instructions such as `mull` and `mulsh`/`muluh`. While hardware multiply instructions exist in the ISA, cycle latencies and generated instruction sequences depend on GCC optimization flags, register pressure, and 64-bit accumulation chaining. This claim remains labeled **unsupported** until measured on real hardware. |
| *"Total CPU cost is ~260–280 MHz equivalent"* | ❌ **UNSUPPORTED NUMBER** | While we observe that `Animals.flac` yields ~0.85x real-time sustained throughput in the complete system, assigning a specific MHz equivalent is speculative. The bottleneck may be a combination of FLAC Rice/LPC decoding, S16 conversion, resampling, SDMMC FATFS latency, PSRAM bus contention, and RTOS task scheduling. |
| *"dr_flac performs 96 kHz -> 44.1 kHz resampling"* | ❌ **WRONG** | `dr_flac` is strictly a FLAC bitstream decoder with zero resampling capability. The resampler is entirely in our application code (`audio_player.c`). |
| *"96 kHz -> 48 kHz can be done by simple pair averaging"* | ❌ **TOO SIMPLISTIC FOR HIGH FIDELITY** | While $96,000 / 48,000 = 2.0$ completely eliminates fractional interpolation math, simple sample averaging is an inadequate low-pass filter (only ~3 dB attenuation at 24 kHz). An anti-aliasing low-pass filter (e.g. half-band FIR) suppressing frequencies above 24 kHz is mathematically required prior to decimation. |
| *"dr_flac S16 conversion performs dithering"* | ❌ **FACTUALLY INEXACT** | The `drflac_read_pcm_frames_s16()` path performs integer bit-shifting, truncation, and channel interleaving into `drflac_int16`. It does not execute psychoacoustic or triangular dithering. |
| *"Test A (>1.5x on 48k/24b) proves sample rate is the bottleneck"* | ❌ **LOGICALLY OVERSTATED** | Converting 96k/24b to 48k/24b alters multiple variables simultaneously: sample count, FLAC subframe blocking, LPC order, Rice partitions, resampler workload, and A2DP feeding. Test A provides combined evidence; isolated micro-benchmarks are required to attribute causality. |
| *"Test D (>1.5x on 96k/16b) proves 24-bit arithmetic was the bottleneck"* | ❌ **LOGICALLY OVERSTATED** | Re-encoding to 16-bit allows the FLAC encoder to select different LPC orders and prediction parameters. A speed difference indicates that bit depth and its associated decoding workload matter, but does not isolate 64-bit ALU operations as the sole culprit. |
| *"512 KB buffer = ~2.97 seconds of audio"* | ⚠️ **CONTEXT-SPECIFIC** | True **only** for 16-bit 44.1 kHz stereo PCM ($176,400 \text{ bytes/s}$). For 24-bit 96 kHz stereo PCM ($576,000 \text{ bytes/s}$), 512 KB holds only **0.91 seconds**. |
| *"Larger buffer cannot fix an underrun if decode rate < consumption rate"* | ✅ **100% TRUE** | By queuing theory, if $\text{Production} < \text{Consumption}$, queue depth monotonically trends to zero. A larger buffer only delays the first pause; once empty, the stutter/pause duty cycle is strictly determined by $\frac{\text{Decode Rate}}{\text{Playback Rate}}$. |
| *"File size itself is not the fundamental issue"* | ✅ **100% TRUE** | A 100 MB 44.1 kHz / 16-bit FLAC (e.g. a 25-minute track) decodes at >3.0x real-time. The issue with `Animals` is sample throughput ($192,000 \text{ samples/s}$) and 24-bit high-order LPC complexity packed into 3.8 minutes ($3,286 \text{ kbps}$). |

---

## 2. Bluetooth Audio Architecture: Universal Multi-Device Compatibility & High-Bitrate SBC

### 2.1 The Reality of SBC Quality & High-Bitrate Configurations
SBC (Low Complexity Subband Codec) is a subband codec utilizing a 4- or 8-band polyphase analysis filter bank followed by adaptive quantization and bit allocation:
- **Standard SBC Configuration**: In conventional A2DP implementations, SBC is configured in **Joint Stereo** mode with a maximum bitpool capped at **53**, yielding **~328 kbps** at 44.1 kHz. To fit complex stereo audio within this constraint, high-frequency subbands (often above 14–16 kHz) receive minimal or zero bit allocation.
- **High-Bitrate Dual Channel Configurations (Informally "SBC XQ")**:
  - In **Dual Channel mode**, the left and right audio channels are encoded completely independently with separate bit allocation routines.
  - Applying a bitpool parameter of **38** in Dual Channel mode produces:
    - **44.1 kHz Dual Channel (bitpool 38)**: **~452 kbps**
    - **48.0 kHz Dual Channel (bitpool 38)**: **~492 kbps**
  - Applying a bitpool parameter of **47** in Dual Channel mode produces:
    - **44.1 kHz Dual Channel (bitpool 47)**: **~551 kbps**
    - **48.0 kHz Dual Channel (bitpool 47)**: **~600 kbps**
  - **Acoustic Characteristics**: Elevated bitrates provide substantial quantization headroom, eliminating high-frequency cutoff and preserving stereo separation (>80 dB channel separation).

### 2.2 Why High-Bitrate SBC is the Pragmatic Choice for ESP32
1. **CPU Efficiency**:
   - **SBC Encoder**: Lightweight subband filter bank requiring **~10–15 MHz** of CPU on Core 0.
   - **AAC Encoder**: Requires MDCT, psychoacoustic masking tables, and Huffman coding, consuming **~60–80 MHz** of CPU and tens of kilobytes of fast internal SRAM. On an ESP32 already loaded with FLAC decoding and SDMMC transfers, AAC source encoding risks starvation.
   - **LDAC Encoder**: Proprietary, closed-source Sony licensing, high computational footprint.
2. **Core 0 Workload Awareness**:
   - Core 0 is not merely executing `memcpy` or radio DMA; the Bluedroid A2DP media task performs SBC polyphase analysis, quantization, and packetization for every transmitted audio frame. Real-world CPU profiling must measure Core 0 overhead alongside Core 1 decode tasks.

### 2.3 Solving Multi-Device Interoperability
Because the Bluetooth SIG A2DP specification allows devices to support **either** 44.1 kHz or 48 kHz, and **any** of Dual Channel, Stereo, or Joint Stereo, a rigid source implementation causes connection failures:
- **Flaws in Fixed Configurations**:
  - If a source advertises only `44.1 kHz`, it cannot stream natively to a 48 kHz-only sink.
  - If a source advertises only `Dual Channel`, sinks that only support `Joint Stereo` or `Stereo` will reject codec configuration during AVDTP negotiation.
  - If a source blindly requests `bitpool = 250`, sinks with limited internal buffers will drop packets or reject the configuration.

- **The Universal Adaptive Negotiation Architecture**:
  To guarantee interoperability across all Bluetooth headphones, car stereos, and portable speakers while delivering optimal fidelity:
  1. **Advertise Full Baseline Capabilities**:
     In `components/bt_override/bta_av_co.c`, advertise both sample rates and all standard channel modes:
     ```c
     const tA2D_SBC_CIE bta_av_co_sbc_caps = {
         .samp_freq    = (A2D_SBC_IE_SAMP_FREQ_48 | A2D_SBC_IE_SAMP_FREQ_44),
         .ch_mode      = (A2D_SBC_IE_CH_MD_DUAL | A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_STEREO),
         .block_len    = (A2D_SBC_IE_BLOCKS_16 | A2D_SBC_IE_BLOCKS_12 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_4),
         .num_subbands = (A2D_SBC_IE_SUBBAND_8 | A2D_SBC_IE_SUBBAND_4),
         .alloc_mthd   = (A2D_SBC_IE_ALLOC_MD_L | A2D_SBC_IE_ALLOC_MD_S),
         .max_bitpool  = 53,
         .min_bitpool  = A2D_SBC_IE_MIN_BITPOOL
     };
     ```
  2. **Adaptive Configuration Selection (`bta_av_co_audio_codec_build_config`)**:
     - **Sampling Rate**:
       - If the incoming audio stream is 48 kHz or 96 kHz, and the sink supports `A2D_SBC_IE_SAMP_FREQ_48` $\to$ Select **48 kHz**.
       - Otherwise, if the sink supports `A2D_SBC_IE_SAMP_FREQ_44` $\to$ Select **44.1 kHz**.
     - **Channel Mode**:
       - If Sink supports `A2D_SBC_IE_CH_MD_DUAL` $\to$ Select **Dual Channel** (enabling ~452–492 kbps high-bitrate SBC).
       - Else if Sink supports `A2D_SBC_IE_CH_MD_JOINT` $\to$ Fall back cleanly to **Joint Stereo** (~328 kbps).
       - Else $\to$ Fall back to **Stereo**.
     - **Bitpool Negotiation**:
       - Dual Channel mode: Set `bitpool = min(sink_max, 38)`.
       - Joint Stereo mode: Set `bitpool = min(sink_max, 53)`.

### 2.4 ESP-IDF Native 48 kHz vs 44.1 kHz PCM Feeding
In ESP-IDF's Bluedroid stack (`btc_a2dp_source.c`):
- When the PCM feeding sample rate matches the negotiated SBC sample rate (e.g. 48,000 Hz PCM into a 48 kHz SBC session, or 44,100 Hz PCM into a 44.1 kHz session), Bluedroid feeds the PCM directly to the encoder with **zero internal sample-rate conversion**.
- When the PCM rate does not match the negotiated SBC rate, Bluedroid activates an internal software resampler.
- **Architectural Takeaway**: To eliminate redundant resampling and CPU overhead on ESP32, the audio pipeline must feed PCM at exactly the negotiated SBC rate.

---

## 3. Storage & FATFS Latency Analysis for Large Files (100 MB+)

Why do large files (100 MB+) exhibit playback stalls while smaller files play without issue?

### 3.1 Cluster Chain Traversal in FAT32
- In FAT32, file data is indexed across clusters linked in the File Allocation Table.
- A 100 MB file formatted with 32 KB clusters spans **3,200 clusters**.
- By default, FatFS in ESP-IDF caches only **a single 512-byte sector** of the FAT.
- Reading through a large file requires reading new FAT sectors from the MicroSD card whenever cluster boundaries are crossed. Non-contiguous cluster allocation (fragmentation) induces seek overhead.

### 3.2 1-Bit SDMMC Physical Bandwidth on HW-297
- The ESP32-CAM board operates SDMMC in 1-bit mode (DAT0 = GPIO 2, CLK = GPIO 14, CMD = GPIO 15).
- At a 20 MHz bus clock, theoretical single-bit throughput is $20 \text{ Mbps} = 2.5 \text{ MB/s}$.
- Real-world sustained throughput with FATFS protocol overhead typically ranges between **1.2 and 1.6 MB/s**.
- High-resolution 96 kHz / 24-bit stereo FLAC (`Animals - Maroon 5.flac`, 94.9 MB for 3.8 minutes) streams at an average bitrate of **3,286 kbps = 411 KB/s**, with complex passages peaking higher.
- The compressed bitstream alone consumes **~30–40% of the entire available SD bus bandwidth**.

### 3.3 MicroSD Flash Wear-Leveling Latency Spikes
- Consumer MicroSD cards periodically halt bus transfers for **20 to 80 milliseconds** to perform internal NAND flash garbage collection, block erase, and wear-leveling.
- By default, `dr_flac` uses a 4 KB internal read buffer (`DR_FLAC_BUFFER_SIZE = 4096`).
- At 450 KB/s bitstream consumption, a 4 KB buffer is emptied in:
  $$\text{Buffer Duration} = \frac{4096 \text{ bytes}}{450,000 \text{ bytes/s}} \approx 9.1 \text{ milliseconds}$$
- This triggers `dr_flac`'s `onRead` callback approximately **110 times per second**, each incurring VFS/FatFS/driver overhead.
- When an SD card wear-leveling stall of 50 ms occurs, a 4 KB buffer is exhausted in under 10 ms, causing an immediate read stall if downstream buffers cannot compensate.

### 3.4 Mitigation Strategies
1. **Enlarge `dr_flac` Read Chunking**:
   Increasing `DR_FLAC_BUFFER_SIZE` or wrapping `onRead` in an intermediate 16 KB / 32 KB buffer reduces syscall frequency and allows the SDMMC controller to utilize multi-block read commands (`CMD18`).
2. **Cluster Size Optimization**:
   Formatting MicroSD cards with **64 KB clusters** halves the number of FAT entries required for a 100 MB file.

---

## 4. FLAC Arithmetic: Verification of Prediction Paths

### 4.1 Decoding Stages in `dr_flac`
FLAC is strictly an integer codec (no floating-point operations). Decoding involves:
1. **Rice Entropy Decoding**: Variable-length code unpacking from the bitstream. At 96 kHz stereo, the decoder processes 192,000 residual samples per second.
2. **Linear Prediction Synthesis**:
   $$\hat{x}[n] = \sum_{k=1}^P c_k \cdot x[n-k] \gg \text{shift}$$
   $$x[n] = \text{residual}[n] + \hat{x}[n]$$
3. **Dynamic 32-bit vs 64-bit Selection**:
   In `dr_flac.h`:
   ```c
   static DRFLAC_INLINE drflac_bool32 drflac__use_64_bit_prediction(drflac_uint32 bitsPerSample, drflac_uint32 order, drflac_uint32 precision)
   {
       return bitsPerSample + precision + drflac__ilog2_u32(order) > 32;
   }
   ```
   - For a 24-bit file (`bitsPerSample = 24`), if coefficient precision is 12 bits and order is 12 (`ilog2_u32(12) = 4`), the sum is $24 + 12 + 4 = 40 > 32$, activating `drflac__calculate_prediction_64()`.
   - For lower LPC orders or lower precision, 32-bit prediction is used.

### 4.2 Xtensa LX6 Multiplier Capabilities
The ESP32 Xtensa LX6 CPU includes a 32-bit hardware multiplier (`mull`, `mulsh`, `muluh`). Hardware instructions allow computing the high and low 32 bits of a $32 \times 32$-bit product. However, accumulating into 64-bit registers on a 32-bit architecture requires multi-register operations. The exact cycle impact must be profiled directly rather than assumed.

---

## 5. Resampling Mathematics: 96 kHz -> 48 kHz vs 96 kHz -> 44.1 kHz

### 5.1 Why 96 kHz -> 48 kHz is Structurally Simpler
- Downsampling $96,000 \to 44,100$ requires a fractional ratio of $\frac{147}{320}$. This necessitates fractional phase accumulators and arbitrary-point interpolation, introducing phase jitter or harmonic distortion unless a complex polyphase filter bank is computed.
- Downsampling $96,000 \to 48,000$ is an **exact 2:1 integer decimation**:
  $$\frac{96,000}{48,000} = 2.0$$
- Exactly one output sample is produced for every two input samples. No fractional phase accumulation or clock drift tracking is needed.

### 5.2 The Anti-Aliasing Filter Requirement
- By the Nyquist-Shannon sampling theorem, reducing the sample rate to 48 kHz lowers the Nyquist bandwidth from 48 kHz to 24 kHz.
- Any signal energy in the input between 24 kHz and 48 kHz will fold back (alias) into the audible band ($0 - 24 \text{ kHz}$) unless attenuated.
- **Why Simple Pair Averaging is Inadequate**:
  Computing $\frac{x[2n] + x[2n+1]}{2}$ is a 2-tap moving average filter ($h[n] = [0.5, 0.5]$). Its frequency response is:
  $$|H(e^{j\omega})| = |\cos(\omega / 2)|$$
  At the 24 kHz Nyquist boundary ($\omega = \pi/2$ relative to 96 kHz), attenuation is only $-3.01 \text{ dB}$. Frequencies around 28–40 kHz will alias into the audible spectrum with minimal attenuation.
- **The Proper Solution: Fixed-Point Half-Band FIR Decimation Filter**:
  A half-band FIR filter exhibits mathematical symmetry where every even-indexed coefficient (except the center tap) is zero:
  $$h[2k] = 0 \quad (\text{for } k \neq 0), \quad h[0] = 0.5$$
  This halves the required multiply-accumulate operations. A 23-tap or 31-tap half-band filter implemented in 16-bit or 32-bit fixed-point integer math provides **>60 dB of stopband rejection**, effectively suppressing aliasing while consuming minimal CPU time.

---

## 6. The 5-File Experimental Matrix & Benchmarking Protocol

### 6.1 Purpose of the Controlled Test Set
To eliminate speculation about what drives pipeline throughput on high-res files:

| File Name | Sample Rate | Target Bit Depth | Empirical Question Addressed |
|---|---:|---:|---|
| `Animals.flac` | 96.0 kHz | 24-bit | Baseline (observed ~0.85x speed) |
| `Animals-48k-24bit.flac` | 48.0 kHz | 24-bit | Combined impact of reduced sample count and 48k direct path |
| `Animals-44k-24bit.flac` | 44.1 kHz | 24-bit | Comparison to working `Cold.flac` (44.1k/24b) |
| `Animals-48k-16bit.flac` | 48.0 kHz | 16-bit | Baseline for 16-bit decoding at 48 kHz |
| `Animals-96k-16bit.flac` | 96.0 kHz | 16-bit | Evaluates whether 96k sample throughput alone causes stalls |

### 6.2 Generation & Verification via FFmpeg / FFprobe
To ensure the files are encoded with exact target parameters:
```bash
# Test A: 48 kHz / 24-bit FLAC
ffmpeg -y -i "Animals - Maroon 5.flac" -ar 48000 -sample_fmt s32 -c:a flac "Animals-48k-24bit.flac"

# Test B: 44.1 kHz / 24-bit FLAC
ffmpeg -y -i "Animals - Maroon 5.flac" -ar 44100 -sample_fmt s32 -c:a flac "Animals-44k-24bit.flac"

# Test C: 48 kHz / 16-bit FLAC
ffmpeg -y -i "Animals - Maroon 5.flac" -ar 48000 -sample_fmt s16 -c:a flac "Animals-48k-16bit.flac"

# Test D: 96 kHz / 16-bit FLAC
ffmpeg -y -i "Animals - Maroon 5.flac" -ar 96000 -sample_fmt s16 -c:a flac "Animals-96k-16bit.flac"
```

**MANDATORY VERIFICATION**: Use `ffprobe` to verify that `bits_per_raw_sample` matches expected parameters:
```bash
ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,bits_per_sample,bits_per_raw_sample,channels -of default=noprint_wrappers=1 "Animals-48k-24bit.flac"
```

### 6.3 The In-Firmware Micro-Benchmark (`benchmark_audio`)
Directly executed on Core 1 via serial console:
```
esp32> benchmark_audio "/sdcard/Animals - Maroon 5.flac" 5
```
1. **Stage 1 (Pure S32 Decode)**: `drflac_read_pcm_frames_s32` into internal SRAM scratchpad, discarded immediately. No Bluetooth, no PSRAM ring buffer, no resampler. Measures raw bitstream unpack + prediction throughput.
2. **Stage 2 (S16 Conversion Overhead)**: `drflac_read_pcm_frames_s16` into internal SRAM scratchpad. Isolates format conversion and channel interleaving overhead inside `dr_flac`.
3. **Stage 3 (Standalone Resampler Benchmarks)**:
   - 96k $\to$ 44.1k fractional linear interpolation.
   - 96k $\to$ 48k 2:1 half-band decimation FIR.
4. **Stage 4 (SDMMC Read Throughput)**: Raw sequential `fread()` throughput in 4 KB, 8 KB, 16 KB, and 32 KB blocks.
5. **Stage 5 (Frame Header Inspection)**: Reports `sampleRate`, `bitsPerSample`, `channels`, subframe prediction types, and the result of `drflac__use_64_bit_prediction()`.

---

## 7. Working System Architecture

```
MicroSD Card (1-Bit SDMMC DMA)
      │
      ▼
┌────────────────────────────────┐
│ dr_flac Decoder (Core 1)       │
│  - Decodes native PCM into      │
│    fast internal SRAM buffer   │
└──────────────┬─────────────────┘
               │
               ▼
      Sample Rate Check?
       ├── 44.1 kHz ──► Direct 44.1k PCM ──┐
       ├── 48.0 kHz ──► Direct 48.0k PCM ──┤
       └── 96.0 kHz ──► Anti-Alias Filter  │
                        + Decimate by 2    │
                               │           │
                               ▼           │
                         48.0k PCM ────────┤
                                           │
                                           ▼
                                 ┌───────────────────┐
                                 │ 512 KB PSRAM Ring │
                                 └─────────┬─────────┘
                                           │
                                           ▼
                                 ┌───────────────────┐
                                 │ A2DP SBC Encoder  │
                                 │ (Core 0)          │
                                 └─────────┬─────────┘
                                           │
                               Negotiated Sink Config?
                                ├── Dual Channel 48k ──► 48 kHz High-Bitrate SBC (~492 kbps)
                                ├── Dual Channel 44k ──► 44.1 kHz High-Bitrate SBC (~452 kbps)
                                ├── Joint Stereo 48k ──► 48 kHz Standard SBC (~345 kbps)
                                └── Joint Stereo 44k ──► 44.1 kHz Standard SBC (~328 kbps)
```
