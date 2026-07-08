# Tone Trixter — SD card template

Drag the **`tonetrix/`** folder onto the root of a FAT32-formatted microSD card, so
the card ends up with:

```
/tonetrix/
  config.txt        global config (boot preset, GR-meter default)
  presets.txt       preset definitions (this template = the 3 built-ins)
  ir/
    tanglewood.wav  48 kHz mono impulse responses
    garrison.wav
    <your own>.wav  drop in any 48 kHz mono WAV (16/24/32-bit or float)
```

## How the pedal uses it

- On boot the pedal mounts the card and, if `/tonetrix/` is present, **the card's
  presets and IRs override the firmware built-ins.** No card (or no `/tonetrix/`
  folder) → the pedal runs on its built-in defaults, unchanged.
- Any WAV in `/tonetrix/ir/` becomes selectable — by name from a preset's `ir:` field
  and in the encoder **IR** menu. Add your own cab/body IRs here.
- Edit `config.txt` / `presets.txt` in any text editor. It's flow-style YAML: flat
  `key: value` lines, inline `[a, b, c]` lists, `#` comments, no indentation traps.

## UART helpers (115200 baud)

- `sdcfg`   — show what was loaded from the card
- `sdir`    — list the IR table (built-in + scanned SD WAVs; `*` = current)
- `sdreload`— re-read the card after editing, without a power cycle

## IR file requirements

- **48 kHz** sample rate (the whole chain runs at 48 kHz; other rates play back at the
  wrong pitch/length — the pedal warns over UART).
- **Mono** preferred (stereo is down-mixed to mono).
- Up to **4096 taps** (~85 ms); longer files are truncated. Only ≤2048 taps is
  bench-validated for CPU headroom — watch `stats` `gap` if you push past that.
