# Tone Trixter

A guitar pedal that makes a piezo pickup sound like a studio microphone — in real-time, on a $7 chip.

**Build log & demos:** [dylangmiles.github.io/pico-tone-trixter](https://dylangmiles.github.io/pico-tone-trixter/) ·
**Video:** [Building a DIY acoustic guitar pedal for great tone](https://www.youtube.com/watch?v=BtMPneFSfpQ)

Piezo pickups are cheap and reliable but have a characteristic harsh, nasal "quack" that EQ alone can't fix. Tone Trixter applies an **acoustic body impulse response** (IR) to the live piezo signal using FFT convolution, transforming it to match a condenser microphone placed in front of the same guitar.

**Status:** V1 prototype **built and gigging** — the full chain (piezo buffer → ES8388 codec → dual-core 2048-tap IR convolution → EQ/compressor → DAC) runs on battery in an enclosure, at 7.1 ms end-to-end latency, and has survived live performance. Current work: an OPA1642 op-amp front end (replacing the JFET buffer) and per-pickup tuning.

---

## How it works

1. Record the same guitar simultaneously through the piezo and a studio microphone
2. Calculate the transfer function between the two signals — this is the IR
3. Apply that IR to the live piezo signal in real-time via FFT convolution

The IR captures the acoustic character of the guitar body and the microphone's response. Apply it and the piezo sounds like the mic was in the room.

---

## Hardware

**Processor:** Raspberry Pi Pico 2 (RP2350, dual Cortex-M33 @ 150 MHz, hardware FPU)
**Codec:** ES8388 (ADC + DAC + programmable-gain amp, I²C-configured, single chip)

**V1 signal chain:**

```
Guitar piezo → buffer daughter board (JFET follower, or OPA1642 op-amp +6 dB)
  → ES8388 ADC (per-preset PGA gain)
    → Pico 2: 2048-tap IR convolution (dual-core) → input trim → 3-band EQ
      → compressor/limiter → output level
    → ES8388 DAC (headphone-amp output)
  → PA / headphones / interface
```

The piezo buffer lives on a swappable 5-pin daughter board — a JFET source-follower or an
OPA1642 FET-input op-amp stage (+6 dB, for the high-impedance passive K&K pickup).
Battery-powered (9 V LiPo → 5 V UBEC), copper-tape-shielded enclosure, TRS in/out.

**UI:** SH1106 OLED + rotary encoder (3-level menu: presets, IR picker, stage params,
codec gain), two footswitches (tuner / DSP bypass), live UART control.

---

## Features

- **2048-tap IR convolution** split across both cores (head on Core 0, tail on Core 1)
- **DSP chain:** input trim → 3-band EQ (RBJ biquads) → compressor/limiter → output level
- **Presets** bundling all params + IR selection + per-preset codec (PGA) gain — switching
  guitars re-stages the whole chain
- **SD card:** drop `tonetrix/` on a card to override presets (`presets.txt`, flow-YAML)
  and add IRs (`ir/*.wav`) without reflashing — see [`sdcard_template/`](sdcard_template/)
- **YIN tuner** (footswitch-toggled, dry monitor)
- **Live UART tuning:** every parameter adjustable while playing (`help` lists commands),
  plus a level meter with ADC- and DAC-clip detection (`meter on`)

---

## DSP Architecture

**Algorithm:** [HiFi-LoFi TwoStageFFTConvolver](https://github.com/HiFi-LoFi/FFTConvolver), split across both cores.

| Core | Role |
|------|------|
| Core 0 | I2S DMA ping-pong, head convolution (64-sample blocks) + DSP chain |
| Core 1 | Tail convolution in background (512-sample segments) |

**IR spec:** 2048 samples at 48 kHz (42.7 ms) — captures guitar body resonance with comfortable CPU headroom.

### Confirmed performance on RP2350 (2048-sample IR)

| Stage | Time per call | Budget | Headroom |
|-------|--------------|--------|----------|
| Core 0 (head) | 0.60 ms | 1.33 ms | **2.24×** |
| Core 1 (tail) | 1.50 ms | 10.7 ms | **7.1×** |

The RP2350's hardware FPU delivers approximately 24× speedup on the tail convolution vs the RP2040 Cortex-M0+ (which ran at 136% of real-time budget — not usable).

---

## IR Capture

IRs are captured by recording the guitar simultaneously through the piezo and a condenser microphone (no effects on either channel), then computing the transfer function via deconvolution.

See [docs/ir_capture_guide.md](docs/ir_capture_guide.md) for the full methodology.

Available IRs (2048 samples @ 48 kHz, NT1-A condenser, UA Gigcaster 8):

| Guitar | IR file |
|--------|---------|
| Garrison acoustic | `audio/samples/IR_garrison-NT1-A-20260320_48k_2048_M.wav` |
| Tanglewood acoustic | `audio/samples/IR_tanglewood-NT1-A-20260320_48k_2048_M.wav` |

---

## Build

Requires Pico SDK 2.x at `$PICO_SDK_PATH` and ARM GCC toolchain.

```sh
cd cmake-build-debug
cmake ..
ninja
```

| Target | What it is |
|--------|------------|
| `pico_tone_trixter` | **The product** — full pipeline (`main.cpp`, runs from RAM) |
| `es8388_codec_bench` | Same code with `ENABLE_IR=0` — pure passthrough A/B reference |
| `offline_test` | IR validation against embedded audio, streamed out over UART |

Output: `cmake-build-debug/pico_tone_trixter.uf2`

Flash via debug probe using CLion's **Debug on Pico** (OpenOCD Download & Run). See `openocd.cfg`.

### Offline IR test

To validate IR processing without live hardware, choose a guitar and build:

```sh
# Garrison acoustic (default)
cmake -DOFFLINE_TEST_TWO_STAGE=ON -DOFFLINE_GUITAR=garrison -DOFFLINE_DURATION=20 ..
ninja offline_test

# Tanglewood acoustic
cmake -DOFFLINE_TEST_TWO_STAGE=ON -DOFFLINE_GUITAR=tanglewood -DOFFLINE_DURATION=20 ..
ninja offline_test
```

Flash `offline_test.uf2`, then capture the processed output over UART:

```sh
# Find the debug probe's UART port
ls /dev/cu.usbmodem*

# Capture — output is saved to tools/output/output_<guitar>-<duration>s-<date>.wav
cd tools && source .venv/bin/activate
python3 capture_wav.py /dev/cu.usbmodem101 --guitar garrison --duration 20
```

Validate the captured output against a Python reference convolution:

```sh
cd tools && source .venv/bin/activate

python3 validate_ir.py \
  --input  ../audio/samples/garrison-piezo-20260320.wav \
  --ir     ../audio/samples/IR_garrison-NT1-A-20260320_48k_2048_M.wav \
  --output output/output_garrison-20s-<date>.wav \
  --plot   output/ir_validation-garrison-<date>.png
```

Six automated checks run: output length, signal modification, spectral shape, and numerical accuracy (target: <2% RMS error vs Python reference). All pass on RP2350.

---

## GPIO Pinout

| GPIO | Function |
|------|----------|
| 0 / 1 | UART0 TX / RX (stdio @ 115200, live control) |
| 2 / 3 / 4 | Rotary encoder SW / B / A (menu) |
| 6 / 8 / 9 / 10 | SD card MISO / CS / SCK / MOSI (bit-bang SPI) |
| 12 | ES8388 DOUT (ADC → Pico, PIO I2S slave) |
| 13 | ES8388 DIN (Pico → DAC, PIO I2S master) |
| 14 / 15 | I²C1 SDA / SCL (ES8388 @ 0x10, SH1106 OLED @ 0x3C) |
| 16 / 17 | I2S BCLK / LRCLK (PIO sideset) |
| 18 / 19 | Footswitches: tuner / DSP bypass (active-low) |
| 21 | ES8388 MCLK (12.288 MHz, CLK_GPOUT0) |
| 22 | Sync-loss scope trigger (debug) |

---

## Project Documentation

- **[Build log (GitHub Pages)](https://dylangmiles.github.io/pico-tone-trixter/)** — the episode-by-episode story, with audio and video
- [docs/ir_capture_guide.md](docs/ir_capture_guide.md) — IR capture methodology
- [docs/wiring.md](docs/wiring.md) — wiring notes
- [sdcard_template/](sdcard_template/) — seed an SD card with presets + IRs

---

*Built in Cape Town, South Africa.*
