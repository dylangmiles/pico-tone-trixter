# pico-tone-trixter

A PWM-based tone player for the Raspberry Pi Pico (RP2040). Plays a looping A4-C5-E5 arpeggio on GPIO 2.

## Debug Output (UART)

`printf` output goes to UART0. Wire your debug probe or USB-UART adapter:

| Pico Pin | Signal | Probe Wire |
|----------|--------|------------|
| GPIO 0   | UART0 TX | Yellow (probe RX) |
| GPIO 1   | UART0 RX | Orange (probe TX) |
| GND      | GND | Black |

Then connect at 115200 baud:

```sh
screen /dev/tty.usbserial* 115200
```

Exit `screen` with `Ctrl-A` then `k`, then `y`.

If using a Picoprobe/debug probe, OpenOCD exposes a virtual UART — connect to it with:

```sh
screen /dev/tty.usbmodem* 115200
```