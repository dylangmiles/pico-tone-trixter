# Wiring — Tone Trixter

Hardware connections between the Raspberry Pi Pico 2 (RP2350) and the ES8388 audio codec module, plus the external bias and attenuation circuitry needed for guitar input.

---

## Pin Mapping

| Pico GPIO | Pico Pin | ES8388 Pin | Function | Notes |
|-----------|----------|------------|----------|-------|
| GP0       | 1        | —          | UART0 TX (stdio @ 115200) | Diagnostic output |
| GP1       | 2        | —          | UART0 RX | |
| GP2       | 4        | —          | PWM test tone (1 kHz) | Optional: for ADC path verification |
| GP5       | 7        | DOUT       | ADC I2S data (ES8388 → Pico) | PIO1 reads this |
| GP6       | 9        | SDA        | I²C data | With ~4.7kΩ pull-up |
| GP7       | 10       | SCL        | I²C clock | With ~4.7kΩ pull-up |
| GP21      | 27       | MCLK       | 12.288 MHz master clock | 100Ω series resistor, short wire |
| GP26      | 31       | DIN        | DAC I2S data (Pico → ES8388) | PIO0 drives this |
| GP27      | 32       | SCLK       | I2S bit clock | Pico is I2S master, 100Ω series |
| GP28      | 34       | LRCLK      | I2S word-select | Pico is I2S master, 100Ω series |
| 3V3 (OUT) | 36       | VDD        | 3.3 V supply | |
| GND       | 38       | GND        | Common ground | Multiple points recommended |

### Why these pins

- **GP0/1 for UART**: default stdio, leaves the rest of the low-numbered pins free.
- **GP21 for MCLK**: one of the dedicated `clock_gpio_init` output-capable pins on RP2350.
- **GP26–28 for I2S out**: PIO0 drives three consecutive pins (DIN, SCLK, LRCLK) — the PIO program requires consecutive pin indices for side-set.
- **GP5 for DOUT input**: chosen to keep the PIO1 input separate from PIO0. ⚠️ It sits next to UART0 on GP0/1 — suspected crosstalk source, see "Known Issues" below.

---

## Block Diagram

```
┌──────────────────┐                            ┌──────────────────┐
│                  │        MCLK (GP21) ────────►                  │
│                  │        SCLK (GP27) ────────►                  │
│                  │        LRCLK (GP28) ───────►                  │
│   Raspberry Pi   │        DIN  (GP26) ────────►   ES8388 Codec   │
│   Pico 2 (RP2350)│◄──── DOUT (GP5)              (module w/ I2C   │
│                  │        SDA  (GP6) ◄────────►   breakout)      │
│                  │        SCL  (GP7) ────────►                   │
│                  │                            │                  │
└──────────────────┘                            └────┬─────────────┘
                                                     │
                                                     ▼
                                             LIN2/RIN2 inputs
                                             (need external bias)
                                                     │
                                           ┌─────────┴─────────┐
                                           │  Bias + Coupling  │
                                           │  (see below)      │
                                           └─────────┬─────────┘
                                                     │
                                             Guitar / signal in
```

---

## External Bias Network (required)

The ES8388 module's `LIN2` / `RIN2` inputs float at ~0.1 V with nothing connected. The ADC expects a DC bias near VDD/2 (≈1.65 V) for AC-coupled audio inputs. Add a simple voltage divider from VDD to GND:

```
 3V3 ────[R1 = 10kΩ]────┬──── LIN2
                        │
                        ├──── RIN2    ← tie both channels to the same node
                        │
                        ├──── (bias node, ≈1.6 V DC)
                        │
                       [R2 = 10kΩ]
                        │
                       GND
```

- **R1 = R2 = 10 kΩ** → mid-point = VDD/2 ≈ 1.65 V. Measured: ~1.6 V in practice (good enough).
- **Current drain**: ~165 µA — negligible.
- **Source impedance** at the bias node: 5 kΩ (R1 ∥ R2).
- Both `LIN2` and `RIN2` wire directly to the bias node — no individual caps.

### Why RIN2 also needs bias

Even though we're using only the left channel for guitar (`ADCCONTROL2 = 0x50` routes LIN2 to left), the ADC runs in stereo mode and samples the right channel too. A floating `RIN2` rails the right-channel PGA, which destabilises the shared I2S state machine. Tying `RIN2` to the same bias node keeps its PGA happy.

---

## Signal Input (AC-coupled to bias node)

To inject an audio signal into the ADC, AC-couple it onto the bias node through a series capacitor. This keeps the 1.6 V DC bias intact while the AC signal superimposes on it.

### Guitar (passive pickup, direct)

```
Guitar tip ────[C = 1µF]──── bias node ──── LIN2
                  │ +                         │
                  │                           │ (direct connection)
   Guitar sleeve ─── GND
```

- **C = 1 µF**. **Ceramic preferred** (non-polar, lower ESR, no leakage). Electrolytic works as a fallback — if used, `+` (long leg) toward the bias node (1.6 V, higher DC); `−` toward the guitar (ground-referenced).
- At 1 µF against the 5 kΩ bias-node impedance, the high-pass corner is ~32 Hz — well below audio.
- Passive pickup signal is small (~50 mVpp for light strumming, up to ~400 mV for hard strums) — no attenuation needed.

### PWM Test Signal (from GP2)

The test firmware generates a 1 kHz square wave on GP2 (3.3 Vpp). This is far too large to inject directly — it would overdrive the ADC by ~100×. Attenuate first:

```
GP2 ────[100kΩ]────┬────[1µF]──── bias node ──── LIN2
                   │     │ +
                  [470Ω] │ (−)
                   │     │
                  GND    │
                         │
         (attenuator         (coupling cap, + to bias node)
          mid-node,
          ~15 mV AC)
```

- **100 kΩ / 470 Ω divider**: ratio 470/(100000 + 470) ≈ 0.0047 → 3.3 Vpp × 0.0047 ≈ **15 mVpp** at the mid-node.
- **1 µF cap**: AC-couples the attenuated signal onto the bias node. **Ceramic preferred** (non-polar, lower ESR, no leakage — cleaner audio path). Electrolytic works as a fallback — if used, `+` toward the bias node.
- Result at LIN2: 1.6 V DC ± ~7.5 mV AC — clean small signal, well within ADC linear range.

---

## Clock Series Resistors

The three I2S clock lines from the Pico to the ES8388 have **100 Ω series resistors** at the Pico side:

```
GP21 ────[100Ω]──── ES8388 MCLK    (12.288 MHz)
GP27 ────[100Ω]──── ES8388 SCLK    (BCLK, 3.072 MHz)
GP28 ────[100Ω]──── ES8388 LRCLK   (48 kHz word-select)
```

- **100 Ω** dampens reflections on these fast-edged clock lines.
- Keep wires **short** (≤ 5 cm ideal) to minimise radiated noise and ringing.
- `gpio_set_drive_strength(MCLK_PIN, GPIO_DRIVE_STRENGTH_2MA)` in code reduces MCLK edge rates further.
- **DIN (GP26)** has no series resistor — it's slow-edged audio data clocked by SCLK, and the receiver (ES8388) samples it on SCLK edges, so reflections matter less.
- **DOUT (GP5 input)** has no series resistor — the ES8388 drives the line; adding resistance here would weaken edges arriving at the Pico's PIO input.

---

## I²C Pull-ups

Standard I²C requires pull-up resistors on SDA and SCL. Many ES8388 breakout modules include these on-board (check your specific module with a multimeter — measure from SDA/SCL to VDD with power off). If not present, add:

```
3V3 ──[4.7kΩ]── SDA
3V3 ──[4.7kΩ]── SCL
```

---

## Ground Notes

- Use the **shortest possible ground path** from the Pico to the ES8388 module.
- Ground the bias network (R2) to the same node as the Pico GND and ES8388 GND.
- Avoid ground loops — use a single star-ground point on the breadboard if possible.

---

## Known Issues

### ADC sync loss after a few seconds
The ADC loses I2S sync after ~5–6 seconds of operation every time. Initial config and first samples work cleanly, then degrade. The noise through headphones has been described as "high-speed modem-like with random aspects."

**Leading theory**: UART crosstalk from GP0/1 into GP5 (DOUT). Every diagnostic `printf` fires a burst at 115200 baud, and cumulative bit errors in the I2S read path eventually desync the PIO reader.

**Mitigations to try**:
- Silence the periodic `printf`s during the audio passthrough (buffer and dump only on sync loss).
- Relocate `DOUT` to a pin further from UART (e.g. GP9 or higher).
- Add a series resistor (~330 Ω) on the DOUT wire to slow edges and reduce radiated noise.

### Why CONTROL1 must be 0x12
Setting `CONTROL1` bit 7 (`0x92`) appears to put the chip into a stuck/dead state (config readback fails, DAC silent). Use `0x12` — see `es8388.cpp` comments and the project memory for context.

---

## Breadboard Layout Tips

- Keep the **1.6 V bias node short** — it's high-impedance (5 kΩ) and susceptible to coupled noise.
- Route **MCLK away from DOUT**. MCLK at 12.288 MHz radiates more than other signals.
- Route **UART traces away from DOUT** (or use a shielded wire for DOUT once we confirm the crosstalk theory).
- Bypass the ES8388 VDD with a **100 nF ceramic + 10 µF electrolytic** close to the VDD pin if the breakout doesn't already have adequate bypass.