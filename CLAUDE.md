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

Real-time guitar pedal: guitar → ES8388 ADC → Pico 2 dual-core 2048-tap acoustic-body
IR convolution → DSP chain (input trim → 3-band EQ → comp/limiter/sustain → output) →
ES8388 DAC. Plus a YIN tuner and runtime-switchable presets, all live-tunable over UART.

### Targets (CMakeLists.txt)

- **`pico_tone_trixter`** — THE product, built from `main.cpp`. `copy_to_ram`.
- **`es8388_codec_bench`** — same `main.cpp` with `ENABLE_IR=0`: pure passthrough, no
  convolver / Core 1 / DSP chain. A/B reference for ES8388 bring-up + raw signal chain.
- `offline_test`, `pcm1808_test`, `passthrough_test` — standalone bench tools.

> History: the product was the `es8388_test` target (audio/es8388_test.cpp) until the
> 2026-06-09 pipeline refactor. A stale sine-fed scaffold (the old main.cpp +
> audio/pipeline.cpp + audio/dsp.cpp — a terminal-scope demo never wired to the ES8388)
> was deleted then. Don't resurrect it.

### File layout

```
main.cpp              — THE product: ES8388 init, PIO/DMA I2S, dual-core IR, the Core 0
                        foreground processing loop, UART command interface
audio/
  es8388.cpp/h        — ES8388 codec I2C init + register helpers (0x18/0x0D = 48 kHz)
  dsp_chain.cpp/h     — configurable Param/Stage chain (in trim, 3-band EQ,
                        comp/limiter/sustain, output) + presets + UART command parser
  biquad.h            — RBJ biquad (HPF / peaking / low+high shelf) for the EQ
  tuner.cpp/h         — YIN pitch detection (tuner mode)
  offline_test.cpp    — standalone IR test: processes embedded audio, streams WAV via UART
  samples/
    IR_{tanglewood,garrison}-NT1-A-...-48k_2048_M.wav — 48 kHz 2048-tap body IRs
    ir_array_{tanglewood,garrison}.h — generated float arrays (do not edit); BOTH are
                        #include'd (each in its own namespace) in main.cpp and are
                        preset-switchable at runtime (the built-in / fallback IRs)
  sd_spi.c/h          — bit-bang SD SPI (GP6/8/9/10); read-only 512-byte sectors
  sd_diskio.c         — FatFs disk-I/O glue → sd_spi
  wav_load.c/h        — on-device WAV → mono float32 (mirrors gen_audio_arrays.py math)
  tt_store.cpp/h      — parse /tonetrix/{config,presets}.txt (flow-YAML) off the card
lib/fatfs/            — FatFs R0.15 (ff.c + ffunicode.c for LFN), read-only, CP437
sdcard_template/      — drag /tonetrix/ onto a card: config.txt + presets.txt (= the 3
                        built-ins) + ir/*.wav. Seeds a card with the current defaults.
i2s/
  i2s.c/h             — PIO I2S output driver, DMA ping-pong; I2S_SAMPLE_RATE / block size
  i2s_out.pio         — PIO: I2S output bit-clocking (Pico is master: BCLK/LRCLK/MCLK)
  i2s_in_slave.pio    — PIO: I2S input, watches BCLK/LRCLK (ES8388 ADC DOUT → Pico)
lib/FFTConvolver/     — HiFi-LoFi FFTConvolver + TwoStageFFTConvolver (head/tail), float
tools/gen_audio_arrays.py — WAV → ir_array_<guitar>.h + piezo_raw.bin (run by CMake)
openocd.cfg           — debug probe config
```

### Dual-core flow (all in main.cpp)

- **Input DMA IRQ (Core 0)**: captures one 256-sample block of ES8388 ADC audio into a
  double buffer, releases a semaphore. Kept light — NO processing in the IRQ (convolving
  in the IRQ smeared the output; that's why it's in the foreground).
- **Core 0 foreground loop**: waits on the block semaphore, runs the IR head convolution
  (TwoStageFFTConvolver) + `dsp_chain_process()`, clips/converts to int32 into a
  publish-indexed staging buffer the output DMA reads. Also polls UART (non-blocking) and
  runs ~1 Hz diagnostics inline (continuous loop — no per-second processing gap).
- **Core 1**: runs ONLY the IR tail FFT — waits on `s_sem_tail_do`, calls the convolver's
  background tail process, releases `s_sem_tail_done` (pre-released once at init to avoid a
  first-block deadlock).
- **FPU Flush-to-Zero** is enabled on BOTH cores — denormals in the decaying IR tail
  otherwise stall the FFT → dropped audio blocks.

### Key constants

| Symbol | File | Value |
|--------|------|-------|
| `I2S_SAMPLE_RATE` | i2s/i2s.h | **48000** — MUST match the ES8388 speed regs (0x18/0x0D) and the IR's own rate. 96 kHz double-speed silently ran the convolver at 2× budget (dropped blocks) and played the 48 kHz IR an octave high — see git log 2026-06-07. |
| `I2S_BLOCK_SIZE` | i2s/i2s.h | 256 mono samples/block (~5.33 ms real-time budget) |
| `IR_HEAD_BLOCK` / `IR_TAIL_BLOCK` | main.cpp | 64 / 512 (full 2048-tap IR; head on Core 0, tail on Core 1) |
| Core 1 stack | main.cpp | 32 KB |

## Critical implementation details

### Float warm-up on Core 0 (legacy note — no longer required)
On RP2040, the `pico_float` ROM used lazy `sf_table` patching that required a warm-up pass on Core 0. On RP2350 the hardware FPU makes this unnecessary, but `main()`'s convolver init still runs two warm-up `process()` calls (harmless) to pre-prime the OLA state.

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

## SD card: on-card presets + IRs (audio/tt_store, wav_load)

At boot, if the card mounts and `/tonetrix/` exists, its config **overrides** the
built-ins; absent card/folder → built-ins stand (silent fallback). It's all-or-nothing
at the file level.

- `/tonetrix/presets.txt` (≥1 valid preset) **replaces** the built-in preset list via
  `dsp_chain_install_presets()`. Unspecified keys in a preset inherit the built-in
  `default` (`dsp_chain_default_preset()`), so partial presets work.
- `/tonetrix/config.txt` → `boot_preset` (loaded at power-on) + `gr_meter` default.
- IR selection includes a synthetic **"none" IR at `s_ir_table[0]`** (`IR_NONE`): selecting
  it turns the convolution stage OFF (the convolver keeps its last real IR loaded but
  bypassed). `main.cpp` tracks `s_cur_ir` (the SELECTED entry, 0=none) vs `s_conv_ir` (the
  real IR actually loaded). A preset's `ir: none`/empty resolves to `IR_NONE`; `ir: <name>`
  selects a real IR and enables convolution (`dsp_chain_set_ir_enabled`). So the display and
  the encoder IR picker both show "none" when dry. The built-in `default` preset is dry
  (ir none) and is preset index 0 / the boot default when no card overrides it.
- `/tonetrix/ir/*.wav` are scanned (`ir_scan_sd`) and appended to `s_ir_table` after the
  two embedded IRs, so presets (`ir:` field) and the encoder IR picker index one table.
  A preset's `ir:` resolves **card-first** (SD file wins over an embedded name of the
  same base), matched case-insensitively ignoring `.wav`.
- Format is **flow-style YAML** (flat `key: value`, inline `[a,b,c]` lists, `---` between
  presets, `#` comments) — valid YAML but no significant indentation. Parser in
  `tt_store.cpp`; host-tested to decode byte-identically to the embedded arrays.
- FatFs LFN is **on** (`FF_USE_LFN=1`, needs `lib/fatfs/ffunicode.c`) so real filenames
  open — with LFN off, `tanglewood.wav` mangles to an unpredictable `TANGLE~1.WAV`.
- **Known limitation**: an SD-IR switch decodes the WAV (blocking bit-bang SD read)
  *inside* the Core 0 foreground safe-switch point — a multi-hundred-ms audio dropout on
  that one switch (embedded-IR switches are instant). Fine at boot / deliberate change;
  move the decode off the audio path if it needs to be seamless. Steady-state audio and
  the embedded path are unaffected.

## UART control + diagnostics (115200, stdio UART)

The product runs quiet (no periodic UART spam) and takes live commands; `help` lists
them. Parsed in `dsp_chain.cpp` (+ `main.cpp` for tuner/meter/preset/ir):

- `<stage>.<param> <val>` — set a param, e.g. `eq.mid_gain 3.5`, `comp.ratio 4`, `in.level 0.30`
- `<stage> on|off` — enable/bypass a stage (`in`, `eq`, `comp`, `out`)
- `preset [name]` — list, or load one (`default`, `tanglewood-slide`, `garrison`) — **also
  switches IR**. `default` is the dry baseline (no IR, convolution off) and is the boot preset.
- `ir [name|none]` — select IR (a name / `none`=off / `on`=re-engage; no arg lists). IR is a
  selection, not a chain stage — there's no `ir` in the stage menu/`dump`. `bypass on|off` —
  kill IR+EQ+comp (output level stays)
- `tuner on|off` — YIN tuner mode (dry monitor; UART needle ~12/s)
- `meter on|off` — live comp gain-reduction + input-level meter (~1/s)
- `dump` — all stages + params + ranges; `stats` — counters
- `sdtest` / `sdpins` — SD bring-up (init+mount+list) / pin float-short test
- `sdcfg` — show on-card config/presets that were loaded; `sdir` — IR table (built-in +
  scanned SD WAVs, `*` = current); `sdreload` — re-read the card after editing (no reboot)
- `stats` fields: `dropped` (foreground skipped a block), `proc`/`tail`/`uart`/`diag`/`gap` µs.
  `gap ≈ proc` with no idle ⟹ CPU-saturated — that decomposition is how the 96 kHz
  sample-rate bug was caught. Budget per block ≈ 5333 µs.

Sync-loss (ADC stuck on a constant) auto-dumps a register/history trace and pulses
`CASCADE_TRIG_PIN` (GPIO 22) for scope triggering.

Live-tuning command replies printf (a ~1-block blip per command) — fine while tuning,
not while performing.

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
| 22 | CASCADE_TRIG_PIN (sync-loss scope trigger, debug) | RIGHT |
| 18 | Footswitch — tuner toggle (active-low, internal pull-up) | (free) |
| 19 | Footswitch — DSP bypass toggle (active-low, internal pull-up) | (free) |