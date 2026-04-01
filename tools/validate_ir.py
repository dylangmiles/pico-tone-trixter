#!/usr/bin/env python3
"""
Validate that the IR is correctly applied by comparing the dry piezo input
to the wet convolved output from the Pico offline_test firmware.

Checks performed:
  1. Length — output is exactly (input_len + ir_len - 1) samples
  2. Identity — dry input ≠ wet output (IR actually changed the signal)
  3. Spectral shape — magnitude spectrum differs between dry and wet
  4. Reference convolution — Python FFT convolution matches Pico output
     within a configurable tolerance (accounts for float32 quantisation
     and 16-bit PCM rounding on the Pico side)
  5. Visualisation — saves a PNG with waveform, spectrum, and diff plots

Usage (activate the tools venv first):
  cd tools
  source .venv/bin/activate
  python3 validate_ir.py [options]

Options:
  --input    PATH   dry input WAV   (default: ../audio/samples/garrison-piezo-20260320.wav)
  --ir       PATH   impulse response (default: ../audio/samples/IR_garrison-NT1-A-20260320_48k_2048_M.wav)
  --output   PATH   wet output WAV  (default: ../output_twostage.wav)
  --plot     PATH   PNG to write     (default: ir_validation.png)
  --tol      FLOAT  max normalised RMS diff for check 4 (default: 0.02 = 2%)
"""

import argparse
import subprocess
import sys
import os
import struct

import numpy as np
import matplotlib
matplotlib.use("Agg")   # headless — write PNG without a display
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def _ffmpeg_to_float32(path: str) -> tuple[np.ndarray, int]:
    """Decode any WAV to float32 mono via ffmpeg. Returns (samples, sample_rate)."""
    # Get sample rate first
    probe = subprocess.run(
        [
            "ffprobe", "-v", "quiet",
            "-select_streams", "a:0",
            "-show_entries", "stream=sample_rate",
            "-of", "csv=p=0",
            path,
        ],
        capture_output=True, text=True, check=True,
    )
    sr_str = probe.stdout.strip()
    if not sr_str.isdigit():
        raise RuntimeError(f"Could not determine sample rate for {path}")
    sample_rate = int(sr_str)

    raw = subprocess.run(
        [
            "ffmpeg", "-v", "quiet",
            "-i", path,
            "-f", "f32le",      # raw float32 little-endian
            "-acodec", "pcm_f32le",
            "-ac", "1",         # force mono
            "-",
        ],
        capture_output=True, check=True,
    ).stdout

    samples = np.frombuffer(raw, dtype="<f4").astype(np.float64)
    return samples, sample_rate


def _int16_wav_to_float64(path: str) -> tuple[np.ndarray, int]:
    """Read a 16-bit PCM WAV produced by the Pico directly (no ffmpeg needed)."""
    raw = subprocess.run(
        [
            "ffmpeg", "-v", "quiet",
            "-i", path,
            "-f", "s16le",
            "-acodec", "pcm_s16le",
            "-ac", "1",
            "-",
        ],
        capture_output=True, check=True,
    ).stdout
    probe_rate = subprocess.run(
        [
            "ffprobe", "-v", "quiet",
            "-select_streams", "a:0",
            "-show_entries", "stream=sample_rate",
            "-of", "csv=p=0",
            path,
        ],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    if not probe_rate.isdigit():
        raise RuntimeError(f"Could not determine sample rate for {path}")
    sample_rate = int(probe_rate)

    samples = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32767.0
    return samples, sample_rate


def _mag_spectrum_db(x: np.ndarray, sr: int, n_fft: int = 8192):
    """Return (freqs_hz, magnitude_dB) for the first half of the FFT."""
    win = np.hanning(min(len(x), n_fft))
    seg = x[:len(win)] * win
    if len(seg) < n_fft:
        seg = np.pad(seg, (0, n_fft - len(seg)))
    S = np.abs(np.fft.rfft(seg, n=n_fft))
    S = np.maximum(S, 1e-12)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    return freqs, 20 * np.log10(S / S.max())


def _reference_convolve(dry: np.ndarray, ir: np.ndarray) -> np.ndarray:
    """Full linear convolution using numpy FFT."""
    n = len(dry) + len(ir) - 1
    nfft = 1
    while nfft < n:
        nfft <<= 1
    nfft <<= 1  # one extra power of 2 for safety
    wet = np.fft.irfft(np.fft.rfft(dry, n=nfft) * np.fft.rfft(ir, n=nfft), n=nfft)
    return wet[:n]


def _normalised_rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x ** 2)))


_failures = []

def _pass(msg):  print(f"  \033[32m[PASS]\033[0m  {msg}")
def _fail(msg):  print(f"  \033[31m[FAIL]\033[0m  {msg}"); _failures.append(msg)
def _fatal(msg): print(f"  \033[31m[FAIL]\033[0m  {msg}"); sys.exit(1)
def _info(msg):  print(f"  \033[34m[INFO]\033[0m  {msg}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input",  default=os.path.join(REPO_ROOT, "audio/samples/garrison-piezo-20260320.wav"))
    ap.add_argument("--ir",     default=os.path.join(REPO_ROOT, "audio/samples/IR_garrison-NT1-A-20260320_48k_2048_M.wav"))
    ap.add_argument("--output", default=os.path.join(REPO_ROOT, "output_twostage.wav"))
    ap.add_argument("--plot",   default=os.path.join(REPO_ROOT, "tools/ir_validation.png"))
    ap.add_argument("--tol",    type=float, default=0.02,
                    help="max normalised RMS diff for reference check (default 0.02 = 2%%)")
    args = ap.parse_args()

    print()
    print("pico-tone-trixter  IR validation")
    print("=" * 50)

    # ------------------------------------------------------------------
    # Load files
    # ------------------------------------------------------------------
    print(f"\nLoading files …")
    for label, path in [("Input (dry)", args.input), ("IR", args.ir), ("Output (wet)", args.output)]:
        if not os.path.exists(path):
            _fatal(f"{label} not found: {path}")
        _info(f"{label}: {path}")

    dry, sr_dry   = _ffmpeg_to_float32(args.input)
    ir,  sr_ir    = _ffmpeg_to_float32(args.ir)
    wet, sr_wet   = _int16_wav_to_float64(args.output)

    _info(f"dry : {len(dry):>7} samples @ {sr_dry} Hz  ({len(dry)/sr_dry:.3f} s)")
    _info(f"ir  : {len(ir):>7} samples @ {sr_ir} Hz  ({len(ir)/sr_ir:.3f} s)")
    _info(f"wet : {len(wet):>7} samples @ {sr_wet} Hz  ({len(wet)/sr_wet:.3f} s)")

    if sr_dry != sr_ir or sr_dry != sr_wet:
        _fatal(f"Sample rate mismatch: dry={sr_dry}  ir={sr_ir}  wet={sr_wet}")

    sr = sr_dry

    # ------------------------------------------------------------------
    # Check 1 — output length = len(dry) + len(ir) - 1
    # ------------------------------------------------------------------
    print("\nCheck 1: output length (should be len(dry) + len(ir) - 1)")
    expected_len = len(dry) + len(ir) - 1
    diff_frames  = len(wet) - expected_len
    if abs(diff_frames) <= 1:      # allow ±1 for rounding in convolver flush
        _pass(f"len(wet)={len(wet)}  expected={expected_len}  diff={diff_frames:+d}")
    else:
        _fail(f"len(wet)={len(wet)}  expected={expected_len}  diff={diff_frames:+d}  (>1 frame tolerance)")

    # ------------------------------------------------------------------
    # Check 2 — wet ≠ dry (IR changed the signal)
    # ------------------------------------------------------------------
    print("\nCheck 2: wet output differs from dry input")
    min_len  = min(len(dry), len(wet))
    rms_dry  = _normalised_rms(dry[:min_len])
    diff_raw = wet[:min_len] - dry[:min_len]
    rms_diff = _normalised_rms(diff_raw)
    rel_diff = rms_diff / (rms_dry + 1e-12)
    _info(f"RMS(dry)={rms_dry:.4f}  RMS(wet−dry)={rms_diff:.4f}  relative={rel_diff:.1%}")
    if rel_diff > 0.01:
        _pass(f"Relative RMS difference {rel_diff:.1%} > 1% — signal was modified")
    else:
        _fail(f"Wet is too similar to dry ({rel_diff:.1%}) — IR may not have been applied")

    # ------------------------------------------------------------------
    # Check 3 — spectral shape differs (qualitative)
    # ------------------------------------------------------------------
    print("\nCheck 3: spectral shape change")
    freqs, dry_db = _mag_spectrum_db(dry, sr)
    _,     wet_db = _mag_spectrum_db(wet, sr)
    spectral_diff = np.mean(np.abs(wet_db - dry_db))
    _info(f"Mean |ΔdB| across spectrum: {spectral_diff:.2f} dB")
    if spectral_diff > 1.0:
        _pass(f"Spectral shape changed by {spectral_diff:.2f} dB mean — IR colouration visible")
    else:
        _fail(f"Spectral shapes too similar ({spectral_diff:.2f} dB) — IR may not be applied")

    # ------------------------------------------------------------------
    # Check 4 — reference convolution match
    # ------------------------------------------------------------------
    print("\nCheck 4: reference convolution (Python FFT vs Pico output)")
    # Use IR as-is — gen_audio_arrays.py stores it without normalization,
    # and offline_test.cpp passes ir_samples directly to convolver.init().
    ref = _reference_convolve(dry, ir)
    # Clamp + quantise to 16-bit (mirror what the Pico does)
    ref_clamp = np.clip(ref, -1.0, 1.0)
    ref_q16   = np.round(ref_clamp * 32767.0) / 32767.0

    # Trim both to same length for comparison
    cmp_len = min(len(ref_q16), len(wet))
    rms_ref = _normalised_rms(ref_q16[:cmp_len])
    rms_err = _normalised_rms(wet[:cmp_len] - ref_q16[:cmp_len])
    rel_err = rms_err / (rms_ref + 1e-12)
    _info(f"RMS(ref)={rms_ref:.4f}  RMS(Pico−ref)={rms_err:.4f}  relative={rel_err:.1%}  tol={args.tol:.1%}")
    if rel_err <= args.tol:
        _pass(f"Pico output matches reference within {rel_err:.1%} (tol={args.tol:.1%})")
    else:
        _fail(f"Pico output deviates {rel_err:.1%} from reference (tol={args.tol:.1%}) — "
              f"possible bug in convolver, IR truncation, or gain mismatch")

    # ------------------------------------------------------------------
    # Check 5 — visualisation
    # ------------------------------------------------------------------
    print(f"\nCheck 5: generating visualisation → {args.plot}")

    t_dry = np.linspace(0, len(dry) / sr, len(dry))
    t_wet = np.linspace(0, len(wet) / sr, len(wet))
    t_ref = np.linspace(0, len(ref_q16) / sr, len(ref_q16))

    fig = plt.figure(figsize=(16, 14), facecolor="#1a1a2e")
    gs  = gridspec.GridSpec(3, 2, figure=fig, hspace=0.45, wspace=0.35,
                            left=0.07, right=0.97, top=0.93, bottom=0.06)

    ACCENT_DRY = "#4fc3f7"
    ACCENT_WET = "#f48fb1"
    ACCENT_REF = "#a5d6a7"
    ACCENT_ERR = "#ffcc80"
    BG         = "#1a1a2e"
    PANEL_BG   = "#16213e"
    TEXT       = "#e0e0e0"

    def _style_ax(ax, title):
        ax.set_facecolor(PANEL_BG)
        ax.set_title(title, color=TEXT, fontsize=10, pad=6)
        ax.tick_params(colors=TEXT, labelsize=8)
        for spine in ax.spines.values():
            spine.set_edgecolor("#444466")
        ax.xaxis.label.set_color(TEXT)
        ax.yaxis.label.set_color(TEXT)

    # --- Row 0, col 0: dry waveform ----------------------------------------
    ax = fig.add_subplot(gs[0, 0])
    ax.plot(t_dry, dry, color=ACCENT_DRY, lw=0.4, alpha=0.85)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("amplitude")
    _style_ax(ax, "Dry input (piezo)")

    # --- Row 0, col 1: wet waveform ----------------------------------------
    ax = fig.add_subplot(gs[0, 1])
    ax.plot(t_wet, wet, color=ACCENT_WET, lw=0.4, alpha=0.85)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("amplitude")
    _style_ax(ax, "Wet output (Pico — IR applied)")

    # --- Row 1, col 0: magnitude spectra overlay ---------------------------
    ax = fig.add_subplot(gs[1, 0])
    ax.plot(freqs / 1000, dry_db, color=ACCENT_DRY, lw=1.0, label="dry")
    ax.plot(freqs / 1000, wet_db, color=ACCENT_WET, lw=1.0, label="wet", alpha=0.85)
    ax.set_xlabel("frequency (kHz)")
    ax.set_ylabel("magnitude (dB, normalised)")
    ax.set_xlim(0, sr / 2000)
    ax.set_ylim(-80, 5)
    ax.legend(fontsize=8, facecolor=PANEL_BG, labelcolor=TEXT, edgecolor="#444466")
    _style_ax(ax, "Magnitude spectrum: dry vs wet")

    # --- Row 1, col 1: spectral difference (wet − dry) ---------------------
    ax = fig.add_subplot(gs[1, 1])
    ax.plot(freqs / 1000, wet_db - dry_db, color=ACCENT_ERR, lw=1.0)
    ax.axhline(0, color="#444466", lw=0.8, ls="--")
    ax.set_xlabel("frequency (kHz)")
    ax.set_ylabel("ΔdB (wet − dry)")
    ax.set_xlim(0, sr / 2000)
    _style_ax(ax, "Spectral difference (wet − dry)")

    # --- Row 2, col 0: Pico vs reference (first 0.5 s) --------------------
    ax = fig.add_subplot(gs[2, 0])
    zoom = int(0.5 * sr)
    ax.plot(t_ref[:zoom], ref_q16[:zoom], color=ACCENT_REF, lw=0.8, label="reference (Python FFT)")
    ax.plot(t_wet[:zoom], wet[:zoom],     color=ACCENT_WET, lw=0.8, label="Pico output", alpha=0.7)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("amplitude")
    ax.legend(fontsize=8, facecolor=PANEL_BG, labelcolor=TEXT, edgecolor="#444466")
    _style_ax(ax, "Reference vs Pico output (first 0.5 s)")

    # --- Row 2, col 1: residual error (Pico − reference) ------------------
    ax = fig.add_subplot(gs[2, 1])
    err = wet[:cmp_len] - ref_q16[:cmp_len]
    t_err = np.linspace(0, cmp_len / sr, cmp_len)
    ax.plot(t_err, err, color=ACCENT_ERR, lw=0.4, alpha=0.9)
    ax.axhline(0, color="#444466", lw=0.8, ls="--")
    ax.set_xlabel("time (s)")
    ax.set_ylabel("error amplitude")
    rms_label = f"RMS err: {rel_err:.2%}"
    ax.text(0.98, 0.95, rms_label, transform=ax.transAxes,
            ha="right", va="top", fontsize=9, color=ACCENT_ERR)
    _style_ax(ax, "Residual error (Pico − reference)")

    # --- Super-title -------------------------------------------------------
    ir_name = os.path.basename(args.ir)
    out_name = os.path.basename(args.output)
    fig.suptitle(
        f"IR validation: {ir_name}\noutput: {out_name}  |  tol={args.tol:.1%}  |  err={rel_err:.2%}",
        color=TEXT, fontsize=11, y=0.975,
    )

    fig.savefig(args.plot, dpi=150, facecolor=BG)
    plt.close(fig)
    _pass(f"Saved: {args.plot}")

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    print("=" * 50)
    if _failures:
        print(f"\033[31m{len(_failures)} check(s) failed:\033[0m")
        for f in _failures:
            print(f"  • {f}")
        print()
        sys.exit(1)
    else:
        print("All checks passed.")
    print()


if __name__ == "__main__":
    main()