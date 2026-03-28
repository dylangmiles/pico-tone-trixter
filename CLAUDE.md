# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment

- **Board**: Raspberry Pi Pico (RP2040), dual-core Cortex-M0+ @ 125 MHz, no hardware FPU
- **SDK**: Pico SDK 1.5.1 at `$PICO_SDK_PATH` (`/Users/dylan/dev/sdk/pico/pico-sdk`) — deliberately pinned to 1.5.1 for RP2040. SDK 2.x will live at a separate path for the RP2350 project.
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
| `I2S_SAMPLE_RATE` | i2s/i2s.h | 44100 |
| `I2S_BLOCK_SIZE` | i2s/i2s.h | 256 (= `DSP_BLOCK_SIZE`) |
| `IR_LENGTH` | audio/dsp.cpp | 512 |
| Core 1 stack | audio/dsp.cpp | 32 KB |

## Critical implementation details

### Float ROM patching (sf_table lazy init)
The RP2040's `pico_float` ROM uses lazy patching on ROM v1 silicon: `sf_table` entries are only patched on first use. Zero inputs to `fmul` short-circuit the code path, leaving entries unpatched. If Core 1 is the first to exercise a float path it can crash.

**Fix**: `dsp_init()` runs two warm-up `process()` calls on Core 0 (first with zeros, then with a sine burst) before launching Core 1. This ensures all `sf_table` entries are patched before Core 1 starts.

### DMA/PIO hardware reset on boot
After a watchdog or flash-triggered reset, DMA and PIO hardware can retain stale state from the bootrom USB stack.

**Fix**: `i2s_output_init()` calls `reset_block(RESETS_RESET_DMA_BITS | RESETS_RESET_PIO0_BITS)` + `unreset_block_wait(...)` before configuring any channels or state machines.

### Core 1 halted by debug probe
When using CLion's "Debug on Pico" (OpenOCD), GDB attaches to Core 0. Core 1 is left halted by the debugger — it reaches `sem_acquire_blocking` but `__wfe()` never returns because it is frozen.

**Fix**: `openocd.cfg` has a `gdb-attach` event that resumes Core 1:
```tcl
rp2040.core1 configure -event gdb-attach {
    catch { targets rp2040.core1; resume; targets rp2040.core0 }
}
```

### NDEBUG for FFTConvolver
`fft_convolver` is built with `NDEBUG` to suppress `assert()`. On newlib/embedded, a failed assert calls `abort()` which spins silently — indistinguishable from a hang.

### copy_to_ram
`pico_set_binary_type(pico_tone_trixter copy_to_ram)` copies the entire binary to SRAM at boot. Eliminates XIP flash latency on Core 1 (Core 1 can't use the XIP cache without extra configuration).

### pico_float + pico_double
`fft_convolver` links both. The Ooura FFT backend uses `double` internally; without `pico_double`, it falls back to catastrophically slow libgcc soft-float.

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

Performance scales linearly with IR segment count (IR_length / block_size):

| Scenario | IR | Segments | FFT size | Real-time |
|---|---|---|---|---|
| Current reverb test | 512 samples | 2 | 1024-pt | 36% |
| NT1-A acoustic IR | 2048 samples | 8 | 512-pt | **8%** |

Bottleneck: Ooura FFT in software float on Cortex-M0+ (no hardware FPU).
RP2350 Cortex-M33 hardware FPU expected to give ~10-20× speedup → ~120% real-time with 2048-sample IR.
`TwoStageFFTConvolver` (already in lib) should give further headroom for long IRs.

## Sample rate note

Current firmware runs at 44100 Hz. IR assets and recorded samples are at **48000 Hz**.
This mismatch must be resolved before real-time deployment — change `I2S_SAMPLE_RATE` to 48000.
The offline test is unaffected (it processes at the IR's native rate).

## GPIO pinout

| GPIO | Function |
|------|----------|
| 0 | UART0 TX (stdio @ 115200) |
| 1 | UART0 RX |
| 26 | I2S DATA |
| 27 | I2S BCLK |
| 28 | I2S LRCLK |