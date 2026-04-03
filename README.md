# Tone Trixter

A guitar pedal that makes a piezo pickup sound like a studio microphone — in real-time, on a $7 chip.

Piezo pickups are cheap and reliable but have a characteristic harsh, nasal "quack" that EQ alone can't fix. Tone Trixter applies an **acoustic body impulse response** (IR) to the live piezo signal using FFT convolution, transforming it to match a condenser microphone placed in front of the same guitar.

**Status:** DSP pipeline validated on RP2350. Prototype hardware (ADC + preamp + DAC) in progress.

---

## How it works

1. Record the same guitar simultaneously through the piezo and a studio microphone
2. Calculate the transfer function between the two signals — this is the IR
3. Apply that IR to the live piezo signal in real-time via FFT convolution

The IR captures the acoustic character of the guitar body and the microphone's response. Apply it and the piezo sounds like the mic was in the room.

---

## Hardware

**Processor:** Raspberry Pi Pico 2 (RP2350, dual Cortex-M33 @ 150 MHz, hardware FPU)

**Prototype signal chain:**

```
Guitar piezo → TL072 preamp → PCM1808 ADC → Pico 2 → CS4344 DAC → output
```

**V1 target:** ES8388 codec (ADC + DAC + programmable gain amp) replaces the discrete preamp and separate ADC/DAC boards. Single-chip audio path, I2C configurable, battery-powered in a Hammond enclosure.

---

## DSP Architecture

**Algorithm:** [HiFi-LoFi TwoStageFFTConvolver](https://github.com/HiFi-LoFi/FFTConvolver), split across both cores.

| Core | Role |
|------|------|
| Core 0 | I2S DMA ping-pong, head convolution (64-sample blocks, low-latency path) |
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

# Garrison
 python3 validate_ir.py \
  --input  ../audio/samples/garrison-piezo-20260320.wav \
  --ir     ../audio/samples/IR_garrison-NT1-A-20260320_48k_2048_M.wav \
  --output output/output_garrison-20s-<date>.wav \
  --plot   output/ir_validation-garrison-<date>.png

# Tanglewood
python3 validate_ir.py \
  --input  ../audio/samples/tanglewood-piezo-20260320.wav \
  --ir     ../audio/samples/IR_tanglewood-NT1-A-20260320_48k_2048_M.wav \
  --output output/output_tanglewood-20s-<date>.wav \
  --plot   output/ir_validation-tanglewood-<date>.png
```

Six automated checks run: output length, signal modification, spectral shape, and numerical accuracy (target: <2% RMS error vs Python reference). All pass on RP2350.

---

## GPIO Pinout

| GPIO | Function |
|------|----------|
| 0 | UART0 TX (stdio @ 115200) |
| 1 | UART0 RX |
| 26 | I2S DATA |
| 27 | I2S BCLK |
| 28 | I2S LRCLK |

---

## Project Documentation

- [docs/ir_capture_guide.md](docs/ir_capture_guide.md) — IR capture methodology

---

*Built in Cape Town, South Africa.*