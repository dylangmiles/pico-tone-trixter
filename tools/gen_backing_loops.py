#!/usr/bin/env python3
"""gen_backing_loops.py -- synthesise drum backing loops for the SD card.

Writes seamless mono 16-bit WAVs to sdcard_template/tonetrix/backing/.

Everything is synthesised from damped sines and shaped noise -- no samples, no
licensing to inherit, and the grooves are editable as data (see STYLES).
Deterministic: the humanisation RNG is seeded per style, so re-running produces
byte-identical files.

Stdlib only, on purpose -- this runs without a venv.

Format: 48 kHz mono 16-bit PCM. 48 kHz because the engine runs at 96 kHz, so
playback is an exact 2x upsample; mono because wav_load_mono_f32() downmixes
anyway; 16-bit to keep the SD reads small (96 kB/s streamed).

  python3 tools/gen_backing_loops.py            # all styles
  python3 tools/gen_backing_loops.py rock       # one style
  python3 tools/gen_backing_loops.py rock --bpm 132 --bars 8
"""
import argparse, math, os, random, struct, sys, wave
from array import array

SR = 48000
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "sdcard_template", "tonetrix", "backing")
PEAK_DBFS = -6.0          # leave headroom; the loop is summed with the guitar

# ---------------------------------------------------------------- primitives

def _lp(buf, cutoff):
    """One-pole low-pass, in place."""
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / SR)
    y = 0.0
    for i, x in enumerate(buf):
        y += a * (x - y)
        buf[i] = y

def _hp(buf, cutoff):
    """One-pole high-pass, in place (x - lowpass(x))."""
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / SR)
    y = 0.0
    for i, x in enumerate(buf):
        y += a * (x - y)
        buf[i] = x - y

def _noise(n, rng):
    return array('f', [rng.uniform(-1.0, 1.0) for _ in range(n)])

def _env(n, attack, decay):
    """Exponential decay with a short linear attack, as a list of gains."""
    at = max(1, int(attack * SR))
    out = array('f', bytes(4 * n))
    for i in range(n):
        a = i / at if i < at else 1.0
        out[i] = a * math.exp(-i / (decay * SR))
    return out

def _mix_into(dst, src, at, gain):
    # `at` MUST NOT go negative: dst[-n] is a silent write to the END of the buffer
    # in Python, which splices an attack transient onto the loop tail and clicks on
    # every repeat. Cost a -9 dBFS seam in folk104 before it was caught.
    src_start = 0
    if at < 0:
        src_start, at = -at, 0
    n = min(len(src) - src_start, len(dst) - at)
    for i in range(n):
        dst[at + i] += src[src_start + i] * gain

# ------------------------------------------------------------------- voices

def v_kick(rng):
    n = int(0.36 * SR)
    out = array('f', bytes(4 * n))
    env = _env(n, 0.0005, 0.085)
    phase = 0.0
    for i in range(n):
        # pitch sweep 120 -> 48 Hz, fast at first: the "thump then body"
        f = 48.0 + 72.0 * math.exp(-i / (0.028 * SR))
        phase += 2.0 * math.pi * f / SR
        out[i] = math.sin(phase) * env[i]
    click = _noise(int(0.004 * SR), rng)
    _hp(click, 1800.0)
    _mix_into(out, click, 0, 0.35)
    return out

def _snare(rng, decay, tone_gain, noise_gain):
    n = int(0.30 * SR)
    out = array('f', bytes(4 * n))
    env = _env(n, 0.0003, decay)
    p1 = p2 = 0.0
    for i in range(n):
        p1 += 2.0 * math.pi * 185.0 / SR
        p2 += 2.0 * math.pi * 330.0 / SR
        out[i] = (math.sin(p1) + 0.7 * math.sin(p2)) * env[i] * tone_gain
    nz = _noise(n, rng)
    _hp(nz, 900.0)
    _lp(nz, 7500.0)
    nenv = _env(n, 0.0002, decay * 1.5)
    for i in range(n):
        out[i] += nz[i] * nenv[i] * noise_gain
    return out

def v_snare(rng):        return _snare(rng, 0.075, 0.55, 0.9)
def v_snare_ghost(rng):  return _snare(rng, 0.030, 0.25, 0.4)

def v_rim(rng):
    n = int(0.07 * SR)
    out = array('f', bytes(4 * n))
    env = _env(n, 0.0002, 0.013)
    p = 0.0
    for i in range(n):
        p += 2.0 * math.pi * 1700.0 / SR
        out[i] = math.sin(p) * env[i] * 0.6
    nz = _noise(n, rng)
    _hp(nz, 2500.0)
    for i in range(n):
        out[i] += nz[i] * env[i] * 0.5
    return out

def _hat(rng, decay, cutoff=6500.0):
    n = int(max(0.09, decay * 4.0) * SR)
    nz = _noise(n, rng)
    _hp(nz, cutoff)
    _hp(nz, cutoff)          # cascade: steeper, more "metallic"
    env = _env(n, 0.0002, decay)
    return array('f', [nz[i] * env[i] for i in range(n)])

def v_hat(rng):       return _hat(rng, 0.018)
def v_hat_open(rng):  return _hat(rng, 0.115)

def v_brush(rng):
    """Brush swirl -- soft band-limited noise with a slow swell."""
    n = int(0.26 * SR)
    nz = _noise(n, rng)
    _hp(nz, 1200.0)
    _lp(nz, 5200.0)
    out = array('f', bytes(4 * n))
    for i in range(n):
        a = i / n
        out[i] = nz[i] * math.sin(math.pi * a) ** 1.6 * 0.5
    return out

def v_brush_hit(rng):
    """Brushed backbeat -- softer, shorter than a struck snare."""
    return _snare(rng, 0.045, 0.18, 0.55)

VOICES = {"kick": v_kick, "snare": v_snare, "ghost": v_snare_ghost,
          "rim": v_rim, "hat": v_hat, "hato": v_hat_open,
          "brush": v_brush, "brushit": v_brush_hit}

# ------------------------------------------------------------------ grooves
# A pattern is {voice: [(step, velocity), ...]} on a grid of `div` steps per bar.
# `swing` shifts every odd step toward a triplet feel (0 = straight, 1 = full).

STYLES = {
    "country_blues": dict(
        fn="blues", bpm=92, bars=4, div=12, swing=0.0,   # 12 = triplet 8ths, shuffle is in the grid
        note="Shuffle in 12/8. Ride-style hat on the long-short triplet pairs.",
        pattern={
            "kick":  [(0, 1.0), (6, 0.85), (10, 0.5)],
            "snare": [(3, 0.95), (9, 1.0)],
            "ghost": [(5, 0.35), (8, 0.3), (11, 0.4)],
            "hat":   [(0, 0.7), (2, 0.45), (3, 0.6), (5, 0.45),
                      (6, 0.7), (8, 0.45), (9, 0.6), (11, 0.45)],
        },
        # Fill replaces hits from `fill_from` onward ONLY -- beats 1-3 keep the
        # groove, so the backbeat never disappears before the loop point.
        fill_from=9,
        fill={"snare": [(9, 0.9), (10, 0.7), (11, 0.95)]}),

    "rock": dict(
        fn="rock", bpm=120, bars=4, div=16, swing=0.0,
        note="Straight 8ths, backbeat on 2 and 4, open hat lift into the turnaround.",
        pattern={
            "kick":  [(0, 1.0), (6, 0.8), (8, 0.9), (14, 0.55)],
            "snare": [(4, 1.0), (12, 1.0)],
            "hat":   [(0, 0.75), (2, 0.5), (4, 0.7), (6, 0.5),
                      (8, 0.75), (10, 0.5), (12, 0.7), (14, 0.5)],
        },
        fill_from=8,
        fill={"snare": [(8, 0.85), (10, 0.8), (12, 0.9), (14, 1.0)],
              "hato":  [(14, 0.6)],
              "kick":  [(8, 0.9)]}),

    "folk_shuffle": dict(
        fn="folksh", bpm=76, bars=4, div=12, swing=0.0,
        note="Slow brushed shuffle in 12/8; swirl on the shuffle 'and'.",
        # div=12 puts the shuffle in the grid itself (triplet 8ths), so no swing
        # offset is needed -- same approach as country_blues.
        pattern={
            "kick":    [(0, 0.95), (6, 0.80)],
            "brushit": [(3, 0.90), (9, 0.95)],
            "brush":   [(2, 0.45), (5, 0.50), (8, 0.45), (11, 0.55)],
            "rim":     [(7, 0.25)],
        },
        fill_from=9,
        fill={"brushit": [(9, 0.90), (11, 0.85)],
              "brush":   [(10, 0.50)]}),

    "country_folk": dict(
        fn="folk", bpm=104, bars=4, div=16, swing=0.12,
        note="Light two-step with brushes: brushed backbeat, swirl on the offbeats.",
        pattern={
            "kick":    [(0, 0.9), (8, 0.85)],
            "brushit": [(4, 0.9), (12, 0.95)],
            "brush":   [(2, 0.5), (6, 0.5), (10, 0.5), (14, 0.55)],
            "rim":     [(7, 0.3), (15, 0.35)],
        },
        fill_from=10,
        fill={"brushit": [(12, 0.85), (14, 0.95)],
              "brush":   [(10, 0.6)]}),
}

# ----------------------------------------------------------------- rendering

def _verify_backbeat(name, buf, bpm, bars, div):
    """Every bar must keep its backbeat -- beats 2 and 4.

    A fill that replaces a whole voice for the final bar silently removes the
    groove's anchor right before the loop point, and the loop then *sounds* like
    it loses time on the wrap even though it is sample-exact. That shipped once.
    """
    sr, spb = SR, 60.0 / bpm * 4.0
    step, half = spb * sr / div, int(0.022 * SR)
    floor = sorted(abs(x) for x in buf)[len(buf) // 2] or 1e-9
    weak = []
    for bar in range(bars):
        for beat, s_ in ((2, div // 4), (4, 3 * div // 4)):
            c = int((bar * div + s_) * step)
            seg = buf[max(0, c - half):min(len(buf), c + half)]
            if not seg or max(abs(x) for x in seg) < floor * 6:
                weak.append(f"bar {bar + 1} beat {beat}")
    if weak:
        print(f"    !! {name}: missing/weak backbeat at {', '.join(weak)} "
              f"-- the loop will sound like it drops time at the wrap")

def _check(name, cfg):
    if not cfg.get("fill"):
        return
    ff = cfg["fill_from"]
    for v, hits in cfg["fill"].items():
        for step, _ in hits:
            assert step >= ff, (f"{name}: fill {v} at step {step} is before fill_from={ff} "
                                f"-- it would double up with the kept pattern hit")

def render(name, cfg, bpm=None, bars=None):
    _check(name, cfg)
    bpm  = bpm  or cfg["bpm"]
    bars = bars or cfg["bars"]
    div, swing = cfg["div"], cfg["swing"]
    rng = random.Random(hash(name) & 0xffff)        # deterministic per style

    spb = 60.0 / bpm * 4.0                          # seconds per bar (4/4)
    loop_n = int(round(spb * bars * SR))
    tail_n = int(1.2 * SR)                          # decay that wraps around
    buf = array('f', bytes(4 * (loop_n + tail_n)))

    # Render each voice once, reuse at every hit.
    bank = {v: VOICES[v](rng) for v in
            set(list(cfg["pattern"]) + list(cfg.get("fill", {})))}

    step_n = spb * SR / div
    for bar in range(bars):
        last = (bar == bars - 1)
        layers = [cfg["pattern"]]
        if last and cfg.get("fill"):
            # Keep every pattern hit BEFORE fill_from -- including the backbeat --
            # and let the fill own the bar from there. Wiping the whole voice for
            # the bar (the original bug) dropped the beat-2 backbeat in all three
            # styles, which reads as the loop losing time at the wrap.
            ff = cfg["fill_from"]
            base = {v: [h for h in hits if h[0] < ff]
                    for v, hits in cfg["pattern"].items()}
            layers = [base, cfg["fill"]]
        for layer in layers:
            for voice, hits in layer.items():
                for step, vel in hits:
                    pos = step
                    if swing and step % 2 == 1:
                        pos += swing * 0.5
                    t = (bar * div + pos) * step_n
                    t += rng.uniform(-0.0016, 0.0016) * SR      # humanise timing
                    t = max(0.0, t)                             # never before the downbeat
                    v = vel * rng.uniform(0.93, 1.0)            # ...and velocity
                    _mix_into(buf, bank[voice], int(t), v)

    # Wrap the overhang so the loop joins itself with no click and no cut decay.
    for i in range(tail_n):
        buf[i] += buf[loop_n + i]

    out = buf[:loop_n]
    dc = sum(out) / len(out)                    # the one-pole filters leave a small offset
    for i in range(loop_n):
        out[i] -= dc
    peak = max(abs(x) for x in out) or 1.0
    gain = (10.0 ** (PEAK_DBFS / 20.0)) / peak
    pcm = array('h', [max(-32768, min(32767, int(x * gain * 32767.0))) for x in out])

    # Short names: LFN is enabled (FF_MAX_LFN 128) so this is not a FAT limit --
    # it is the SH1106's 128 px row in the selection list. Kept <= 8 chars so the
    # names also survive a bare 8.3 filesystem.
    _verify_backbeat(name, out, bpm, bars, div)

    fn = f"{cfg['fn']}{bpm}.wav"
    path = os.path.join(OUT_DIR, fn)
    os.makedirs(OUT_DIR, exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"  {fn:28s} {bars} bars @ {bpm} bpm  "
          f"{loop_n/SR:5.2f} s  {len(pcm)*2/1024:6.0f} kB  -- {cfg['note']}")
    return path

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("styles", nargs="*",
                    help="styles to render (default: all): " + ", ".join(STYLES))
    ap.add_argument("--bpm", type=int, help="override tempo")
    ap.add_argument("--bars", type=int, help="override loop length in bars")
    a = ap.parse_args()
    names = a.styles or list(STYLES)
    for n in names:
        if n not in STYLES:
            ap.error(f"unknown style {n!r} -- choose from {', '.join(STYLES)}")
    print(f"Rendering {len(names)} loop(s) to sdcard_template/tonetrix/backing/ "
          f"({SR} Hz mono 16-bit, peak {PEAK_DBFS:.0f} dBFS)")
    for n in names:
        render(n, STYLES[n], a.bpm, a.bars)

if __name__ == "__main__":
    sys.exit(main())
