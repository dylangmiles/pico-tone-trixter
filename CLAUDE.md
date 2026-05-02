# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment

- **Board**: Raspberry Pi Pico 2 (RP2350), dual-core Cortex-M33 @ 150 MHz, hardware FPU
- **SDK**: Pico SDK 2.x at `$PICO_SDK_PATH` (`/Users/dylan/dev/sdk/pico/pico-sdk-2`). Old SDK 1.5.1 remains at `/Users/dylan/dev/sdk/pico/pico-sdk` for reference.
- **Toolchain**: ARM GCC at `/Applications/ArmGNUToolchain/12.3.rel1/arm-none-eabi/`
- **Build system**: CMake + Ninja (CLion-managed, build dir is `cmake-build-debug`)
- **Flash**: Debug probe via CLion "Debug on Pico" (OpenOCD Download & Run)

## Build

```sh
cd cmake-build-debug
cmake ..
ninja
```

Output UF2: `cmake-build-debug/pico_tone_trixter.uf2`

## Architecture

Real-time audio DSP pipeline with FFT convolution reverb.

### File layout

```
main.cpp              — entry point, frame loop (Core 0)
audio/
  pipeline.cpp/h      — DMA callback, sine oscillator, shared buffers, semaphores
  dsp.cpp             — FFTConvolver init + warm-up, Core 1 entry point
  offline_test.cpp    — standalone IR test: processes embedded audio, streams WAV via UART
  samples/
    IR_garrison-NT1-A-20260320_48k_2048_M.wav  — 2048-sample NT1-A acoustic IR, 48kHz
    garrison-piezo-20260320.wav                 — 7.9 sec piezo recording, 48kHz
    ir_array.h          — generated: IR as C float array (do not edit)
    piezo_raw.bin       — generated: raw float32 samples for objcopy embedding
i2s/
  i2s.c/h             — PIO I2S output driver, DMA ping-pong
  i2s_out.pio         — PIO program: I2S bit-clocking
lib/FFTConvolver/     — HiFi-LoFi FFTConvolver (Ooura FFT backend)
  AudioFFT.cpp        — converted from double → float for speed
tools/
  gen_audio_arrays.py — converts WAV files → ir_array.h + piezo_raw.bin (run by CMake)
  capture_wav.py      — host script: receives processed WAV from Pico over UART
  requirements.txt    — pyserial (install into tools/.venv)
openocd.cfg           — debug probe config
```

### Dual-core flow

- **Core 0**: DMA IRQ fires every 256 samples (~5.8 ms). Fills DSP input from sine oscillator, signals Core 1 via `g_sem_input_ready`, reads Core 1 output via `g_sem_output_ready`, converts float→I2S int32.
- **Core 1**: Waits on `g_sem_input_ready`, runs `FFTConvolver::process()`, releases `g_sem_output_ready`.

### Key constants

| Symbol | File | Value |
|--------|------|-------|
| `I2S_SAMPLE_RATE` | i2s/i2s.h | 48000 |
| `I2S_BLOCK_SIZE` | i2s/i2s.h | 256 (= `DSP_BLOCK_SIZE`) |
| `IR_LENGTH` | audio/dsp.cpp | 512 |
| Core 1 stack | audio/dsp.cpp | 32 KB |

## Critical implementation details

### Float warm-up on Core 0 (legacy note — no longer required)
On RP2040, the `pico_float` ROM used lazy `sf_table` patching that required a warm-up pass on Core 0. On RP2350 the hardware FPU makes this unnecessary, but `dsp_init()` still runs two warm-up `process()` calls (harmless) to pre-prime the OLA convolver state.

### DMA/PIO hardware reset on boot
After a watchdog or flash-triggered reset, DMA and PIO hardware can retain stale state from the bootrom USB stack.

**Fix**: `i2s_output_init()` calls `reset_block(RESETS_RESET_DMA_BITS | RESETS_RESET_PIO0_BITS)` + `unreset_block_wait(...)` before configuring any channels or state machines.

### Core 1 halted by debug probe
When using CLion's "Debug on Pico" (OpenOCD), GDB attaches to Core 0. Core 1 is left halted by the debugger — it reaches `sem_acquire_blocking` but `__wfe()` never returns because it is frozen.

**Fix**: `openocd.cfg` has a `gdb-attach` event that resumes Core 1:
```tcl
rp2350.core1 configure -event gdb-attach {
    catch { targets rp2350.core1; resume; targets rp2350.core0 }
}
```

### NDEBUG for FFTConvolver
`fft_convolver` is built with `NDEBUG` to suppress `assert()`. On newlib/embedded, a failed assert calls `abort()` which spins silently — indistinguishable from a hang.

### copy_to_ram
`pico_set_binary_type(pico_tone_trixter copy_to_ram)` copies the entire binary to SRAM at boot. Eliminates XIP flash latency on Core 1. RP2350 has 520 KB SRAM so this is comfortable.

## Diagnostics

The frame loop (Core 0, every 400 ms) prints:
```
frame=N  blocks=N  cp=N  dma_irq=N
```

| Field | Meaning |
|-------|---------|
| `blocks` | Core 1 completed process() calls |
| `cp` | Core 1 checkpoint (2=waiting sem, 10=reading buf idx, 11=in process(), 12=process() returned) |
| `dma_irq` | DMA IRQ0 handler call count |

At real-time: `blocks` ≈ `dma_irq` ≈ 172 per 400 ms.

## Performance status

RP2040 measurements (for reference):

| Scenario | IR | Segments | FFT size | Real-time (RP2040) |
|---|---|---|---|---|
| Synthetic reverb | 512 samples | 2 | 1024-pt | 36% |
| NT1-A acoustic IR | 2048 samples | 8 | 512-pt | **8%** |

RP2350 hardware FPU (Cortex-M33) expected ~10-20× float speedup. Projected with 2048-sample IR:
- `FFTConvolver` block=256: ~120% of real-time → too tight
- `TwoStageFFTConvolver` head=64 tail=512: Core 0 ~0.75ms/block (budget 1.33ms), Core 1 ~2.4ms/call (budget 10.7ms) → comfortable headroom

`TwoStageFFTConvolver` is in `lib/FFTConvolver/` and selectable via `OFFLINE_TEST_TWO_STAGE=ON`.

## GPIO pinout

Reallocated 2026-05-01 to optimise proto-board wiring. All audio data lines (DOUT, DIN, SDA, SCL) now on Pico LEFT side; all clocks (BCLK, LRCLK, MCLK) on RIGHT side. See `pico-tone-trixter-private/docs/proto_board_layout_2026-05-01.md` for routing rationale.

| GPIO | Function | Side (mirrored bottom view) |
|------|----------|---|
| 0 | UART0 TX (stdio @ 115200) | LEFT |
| 1 | UART0 RX | LEFT |
| 12 | ES8388 DOUT (ADC I2S input → Pico) | LEFT |
| 13 | I2S DATA / DIN (DAC out → ES8388 DIN) | LEFT |
| 14 | ES8388 SDA (I²C1 SDA — silicon-fixed even pin) | LEFT |
| 15 | ES8388 SCL (I²C1 SCL — silicon-fixed odd pin) | LEFT |
| 16 | I2S BCLK / ES8388 SCLK | RIGHT |
| 17 | I2S LRCLK / ES8388 LRCLK (= BCLK + 1, PIO sideset) | RIGHT |
| 21 | ES8388 MCLK (12.288 MHz, hardware CLK_GPOUT0, 100Ω series) | RIGHT |
| 22 | CASCADE_TRIG_PIN (debug scope trigger, es8388_test only) | RIGHT |