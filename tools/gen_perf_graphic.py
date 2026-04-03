"""
Generate RP2040 vs RP2350 performance comparison graphic for Article 1.
"""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ── Data ──────────────────────────────────────────────────────────────────────
# RP2040: 136% of real-time budget (36% over — not usable)
# RP2350 Core 0: 0.60ms used / 1.33ms budget = 45.1%  → 2.24× headroom
# RP2350 Core 1: 1.50ms used / 10.7ms budget = 14.0%  → 7.1×  headroom

bars = [
    dict(label="RP2040\n(Cortex-M0+\nno FPU)", pct=136, budget=100,
         color="#e05c5c", note="136% of budget\n36% OVER — not usable"),
    dict(label="RP2350\nCore 0\n(head, 64-sample)", pct=45.1, budget=100,
         color="#5cb85c", note="45% of budget\n2.24× headroom"),
    dict(label="RP2350\nCore 1\n(tail, background)", pct=14.0, budget=100,
         color="#5cb85c", note="14% of budget\n7.1× headroom"),
]

# ── Style ─────────────────────────────────────────────────────────────────────
DARK_BG  = "#1a1a2e"
PANEL_BG = "#16213e"
GRID     = "#2a2a4a"
TEXT     = "#e0e0e0"
ACCENT   = "#e0e050"   # yellow for the budget line label

plt.rcParams.update({
    "figure.facecolor":  DARK_BG,
    "axes.facecolor":    PANEL_BG,
    "axes.edgecolor":    GRID,
    "axes.labelcolor":   TEXT,
    "xtick.color":       TEXT,
    "ytick.color":       TEXT,
    "text.color":        TEXT,
    "grid.color":        GRID,
    "font.family":       "sans-serif",
    "font.size":         11,
})

fig, ax = plt.subplots(figsize=(9, 5.5))
fig.patch.set_facecolor(DARK_BG)

x = np.arange(len(bars))
width = 0.5

for i, b in enumerate(bars):
    bar = ax.bar(i, b["pct"], width, color=b["color"], zorder=3,
                 linewidth=0, alpha=0.92)
    # value label inside / above bar
    va = "bottom"
    y_txt = b["pct"] + 2
    if b["pct"] > 100:
        # bar overflows the budget line — put note below the overflow cap
        y_txt = b["pct"] + 3
    ax.text(i, y_txt, b["note"], ha="center", va="bottom",
            color=TEXT, fontsize=9.5, linespacing=1.4)

# Budget line at 100%
ax.axhline(100, color=ACCENT, linewidth=1.6, linestyle="--", zorder=4)
ax.text(len(bars) - 0.5 + 0.02, 101.5, "real-time budget (100%)",
        color=ACCENT, fontsize=9, ha="right", va="bottom")

# Overflow cap: clip the red bar visually at ~155 and draw a "burst" indicator
ax.set_ylim(0, 155)

ax.set_xticks(x)
ax.set_xticklabels([b["label"] for b in bars], fontsize=10.5)
ax.set_ylabel("% of real-time budget used", fontsize=11)
ax.set_title("RP2040 vs RP2350 — 2048-sample IR, real-time performance",
             fontsize=13, fontweight="bold", pad=14)

ax.yaxis.grid(True, zorder=0)
ax.set_axisbelow(True)

# Shade the over-budget zone
ax.axhspan(100, 155, color="#e05c5c", alpha=0.08, zorder=0)

# Shade the comfortable zone
ax.axhspan(0, 100, color="#5cb85c", alpha=0.04, zorder=0)

ax.spines["top"].set_visible(False)
ax.spines["right"].set_visible(False)

fig.tight_layout()

out = "tools/output/perf_comparison.png"
fig.savefig(out, dpi=150, bbox_inches="tight", facecolor=DARK_BG)
print(f"Saved: {out}")