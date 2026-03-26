# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment

- **Board**: Raspberry Pi Pico (RP2040)
- **SDK**: Pico SDK 1.5.1 at `$PICO_SDK_PATH` (`/Users/dylan/dev/sdk/pico/pico-sdk`)
- **Toolchain**: ARM GCC at `/Applications/ArmGNUToolchain/12.3.rel1/arm-none-eabi/`

## Build

```sh
mkdir -p build && cd build
cmake ..
make
```

The output `.uf2` file (for drag-and-drop flashing) will be at `build/pico_tone_trixter.uf2`.

## Architecture

Single-file C project. `main.c` contains:

- `play_tone(gpio, freq_hz, duration_ms)` — configures PWM on the given GPIO pin to produce a square wave at the specified frequency, blocks for the duration, then silences the pin.
- `main()` — loops forever playing a simple A4-C5-E5 arpeggio.

Audio output is PWM-driven (50% duty cycle square wave) on `AUDIO_PIN` (GPIO 2). The PWM clock divider is computed from the 125 MHz system clock to hit the target frequency with a 4096-step wrap.

UART stdio is enabled; USB stdio is disabled. `printf` output goes to UART0 TX (GPIO 0) at 115200 baud. GPIO 1 is UART0 RX. `AUDIO_PIN` was moved to GPIO 2 to avoid conflict with UART0.
