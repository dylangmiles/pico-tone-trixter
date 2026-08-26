# Backing tracks

Drum beds for playing along. Select with the OLED `BK:` menu row, or over UART:

    bk                 list (* = playing)
    bk 0 | bk blues92  play by index or name
    bk off             stop
    bk level 0.8       level, independent of out.level
    bk stat            ring fill, underruns, worst service time

Loops are **pure data** — drop a new WAV in this folder and `bk scan` picks it up.
No reflash. Any rate, 8/16/24/32-bit or float, mono or stereo; it is resampled to the
engine rate on the fly. These are 48 kHz mono 16-bit, peak −6 dBFS to leave headroom
for the guitar summed on top.

## Beat 1 is marked with a bell

Every bar's **1** carries a short inharmonic bell (~2.1 kHz, ~55 ms). It is
deliberately not a kit voice: a few dB of accent is easy to lose once a guitar is on
top, so the downbeat differs in *timbre*, not level. Inharmonic rather than a pure
tone, so it reads as percussion instead of a note that fights whatever key you are in.

Turn it down or off when you no longer need it — see *Regenerating* below.

## The loops

⚠ **Song suggestions are starting points, not matches.** These are generic grooves in
the right style — never reproductions. The tempos were chosen from where each style
usually sits, **not measured against any recording**, so expect to want a nudge.

| File | bpm | Feel | Try it with |
|---|---|---|---|
| `folksh76`  |  76 | Slow brushed shuffle, 12/8, swirl on the shuffle "and" | slow 12/8 ballads, gentle blues |
| `blues92`   |  92 | Country blues shuffle, 12/8, ride-style hat, ghost notes | acoustic country blues, slide |
| `folk104`   | 104 | Light two-step with brushes, cross-stick | country folk, fingerstyle |
| `rock120`   | 120 | Straight 8ths, backbeat, open-hat lift into the turnaround | general rock |
| `folkr128`  | 128 | Gentle country-folk-rock, soft brushed backbeat, sparse kick | **"Old Man" — Neil Young** *(requested)* |
| `rkbly138`  | 138 | Rockabilly, handclaps on the backbeat, more bounce | slower take on the below |
| `soul140`   | 140 | Soul-pop with a little pocket | slower take on the below |
| `soul150`   | 150 | Bright uptempo soul-pop, full kit, driving 8ths | **"Brown Eyed Girl" — Van Morrison** *(requested)* |
| `rkbly156`  | 156 | Rockabilly bounce, handclaps doubling the backbeat | **"Crazy Little Thing Called Love" — Queen** *(requested)* |

All are **4 bars**, seamless, and start exactly on beat 1.

## Regenerating

Everything here is synthesised by `tools/gen_backing_loops.py` in the firmware repo —
no samples, nothing licensed. Grooves are data (step/velocity lists in `STYLES`), so
changing a pattern is an edit, not new code.

    python3 tools/gen_backing_loops.py                     # all styles, defaults
    python3 tools/gen_backing_loops.py rock --bpm 132      # retune one
    python3 tools/gen_backing_loops.py soul_pop --bpm 140 --swing 0.06
    python3 tools/gen_backing_loops.py rock --ping 0       # no beat-1 bell

Knobs: `--bpm`, `--swing` (0 = straight, ~0.33 = full triplet), `--accent` (beat-1
level), `--ping` (beat-1 bell level, 0 = off). The render line prints all four, so a
re-render says what it did.

⚠ **The tempo is part of the filename.** Re-rendering at a new tempo writes a *new*
file rather than replacing the old one — delete the stale one or you will have both
in the picker.

## Loading the card

    cp .../sdcard_template/tonetrix/backing/*.wav /Volumes/<CARD>/tonetrix/backing/
    dot_clean -m /Volumes/<CARD>/tonetrix

`dot_clean` strips the `._name.wav` sidecars macOS writes on FAT volumes. The firmware
ignores them, but they clutter the card.
