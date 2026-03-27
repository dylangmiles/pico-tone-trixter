# pico-tone-trixter

Real-time audio DSP on the Raspberry Pi Pico (RP2040). Generates a 440 Hz sine wave, passes it through an FFT convolution reverb engine running on Core 1, and outputs stereo I2S audio.

## Architecture

### Dual-core pipeline

| Core | Role |
|------|------|
| Core 0 | I2S DMA ping-pong, sine wave generation, IRQ handling |
| Core 1 | FFT convolution (HiFi-LoFi FFTConvolver, Ooura backend) |

Core 0 fills a DSP input block each DMA completion and releases a semaphore. Core 1 wakes, runs the convolver, and releases the output semaphore. Core 0 reads the output and converts it to I2S format.

### Signal chain

```
Sine oscillator (440 Hz)
  → FFT convolution reverb (IR_LENGTH=512 samples @ 44100 Hz)
    → I2S stereo output (32-bit, 44100 Hz)
```

### Key parameters

| Parameter | Value |
|-----------|-------|
| Sample rate | 44,100 Hz |
| Block size | 256 samples |
| IR length | 512 samples (~11.6 ms) |
| FFT size | 1024-point (Ooura) |
| I2S word width | 32-bit stereo |

## GPIO pinout

| GPIO | Function |
|------|----------|
| 0 | UART0 TX (stdio) |
| 1 | UART0 RX |
| 26 | I2S DATA |
| 27 | I2S BCLK |
| 28 | I2S LRCLK |

## Build

Uses CLion's cmake-build-debug directory with ninja:

```sh
cd cmake-build-debug
cmake ..
ninja
```

Output: `cmake-build-debug/pico_tone_trixter.uf2`

## Flashing

Via debug probe (Raspberry Pi Debug Probe / CMSIS-DAP) using CLion's **Debug on Pico** (OpenOCD Download & Run) configuration. See `openocd.cfg`.

The `openocd.cfg` includes a `gdb-attach` hook that resumes Core 1 after the GDB session starts — without it, Core 1 is left halted by the debugger and the pipeline stalls.

## Debug output (UART)

Connect at 115200 baud via the debug probe's virtual UART:

```sh
screen /dev/tty.usbmodem* 115200
```

The main loop prints `frame`, `blocks` (Core 1 completions), `cp` (Core 1 checkpoint), and `dma_irq` (DMA IRQ count) every 400 ms.

## Performance

| Metric | Value |
|--------|-------|
| Required | 172 blocks/sec (real-time) |
| Current | ~62 blocks/sec (~36% real-time) |

The gap is due to the Ooura FFT running in software float on a no-FPU Cortex-M0+. Optimisation is in progress.