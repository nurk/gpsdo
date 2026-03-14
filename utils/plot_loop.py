#!/usr/bin/env python3
"""
plot_loop.py — GPSDO PI-loop visualiser
Usage:  python3 utils/plot_loop.py logs/<logfile>.log
        python3 utils/plot_loop.py logs/<logfile>.log --save  # save PNG instead of showing

Panels (all vs time in seconds):
  1. Phase error — EMA-filtered + raw TIC corrected net value; lock threshold lines
  2. EFC output  — I Accumulator (slow setpoint) + DAC output (I + P) in volts
  3. P-term + Coarse Trim — proportional kick each tick; coarse trim steps on secondary axis
  4. Timer Counter Error + Coarse Error Accumulator — frequency error driving coarse trim
  5. Temperature — OCXO and board sensors in °C

Vertical annotations:
  • WARMUP → RUN transition
  • LOCKED declaration

Usage:
# Show interactive window
python3 utils/plot_loop.py logs/2026-03-14-run10.log

# Save a PNG next to the log file
python3 utils/plot_loop.py logs/2026-03-14-run10.log --save

"""

import sys
import os
import re
import argparse
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.ticker as ticker

# ── Argument parsing ──────────────────────────────────────────────────────────

parser = argparse.ArgumentParser(description="Plot GPSDO loop data from a serial log file.")
parser.add_argument("logfile", help="Path to the .log file")
parser.add_argument("--save", action="store_true",
                    help="Save PNG next to the log file instead of opening a window")
parser.add_argument("--f", action="store_true", help="fullscreen")
args = parser.parse_args()

if not os.path.isfile(args.logfile):
    print(f"Error: file not found: {args.logfile}", file=sys.stderr)
    sys.exit(1)

# ── Parse log ────────────────────────────────────────────────────────────────

FIELD_RE = re.compile(r'([^,]+?):\s*([^,]+)')


def parse_line(line):
    """Return dict of field→value from one log line, or None if not a data line."""
    if not line.startswith("Time:"):
        return None
    fields = {}
    for m in FIELD_RE.finditer(line):
        fields[m.group(1).strip()] = m.group(2).strip()
    return fields


records = []
lock_time = None
run_start_time = None
prev_mode = None

with open(args.logfile) as f:
    lines = f.readlines()

i = 0
while i < len(lines):
    raw = lines[i].rstrip()
    if raw == "LOCKED" and i + 1 < len(lines):
        nxt = parse_line(lines[i + 1].rstrip())
        if nxt and lock_time is None:
            lock_time = int(nxt["Time"])
        i += 1
        continue
    d = parse_line(raw)
    if d:
        mode = int(d.get("Mode", 2))
        t_val = int(d["Time"])
        if prev_mode == 2 and mode == 0 and run_start_time is None:
            run_start_time = t_val
        prev_mode = mode

        coarse_trim = float(d.get("Coarse Trim", 0))
        records.append({
            "t": t_val,
            "mode": mode,
            "filtered": float(d.get("TIC Corrected Net Value Filtered", 0)),
            "raw": float(d.get("TIC Corrected Net Value", 0)),
            "acc": float(d.get("I Accumulator", 0)),
            "dac": int(d.get("DAC Value", 0)),
            "pterm": float(d.get("P term", 0)),
            "delta": float(d.get("TIC delta", 0)),
            "lockcount": int(d.get("PPS Lock count", 0)),
            "tc_error": int(d.get("Timer Counter Error", 0)),
            "freq_error": float(d.get("TIC Frequency Error", 0)),
            "coarse_trim": coarse_trim,
            "coarse_accum": float(d.get("Coarse Error Accumulator", 0)),
            "temp_ocxo": float(d.get("Temp OCXO", 0)) if "Temp OCXO" in d else None,
            "temp_board": float(d.get("Temp Board", 0)) if "Temp Board" in d else None,
        })
    i += 1

if not records:
    print("No data records found — check the log file format.", file=sys.stderr)
    sys.exit(1)

# ── Unpack into lists ─────────────────────────────────────────────────────────

t = [r["t"] for r in records]
filtered = [r["filtered"] for r in records]
raw_tic = [r["raw"] for r in records]
acc = [r["acc"] for r in records]
dac = [r["dac"] for r in records]
pterm = [r["pterm"] for r in records]
delta = [r["delta"] for r in records]
mode = [r["mode"] for r in records]
lockcount = [r["lockcount"] for r in records]
tc_error = [r["tc_error"] for r in records]
freq_error = [r["freq_error"] for r in records]
coarse_trim = [r["coarse_trim"] for r in records]
coarse_accum = [r["coarse_accum"] for r in records]
temp_ocxo = [r["temp_ocxo"] for r in records]
temp_board = [r["temp_board"] for r in records]

has_temp = any(v is not None for v in temp_ocxo)

# DAC / accumulator to voltage
dac_v = [d / 65535 * 5.0 for d in dac]
acc_v = [a / 65535 * 5.0 for a in acc]

# Coarse trim: only show ticks where a trim was actually applied (non-zero)
coarse_trim_t = [t[i] for i in range(len(t)) if coarse_trim[i] != 0.0]
coarse_trim_v = [coarse_trim[i] for i in range(len(t)) if coarse_trim[i] != 0.0]

# ── Colour scheme ─────────────────────────────────────────────────────────────

C_FILTERED = "#1f77b4"  # blue
C_RAW = "#aec7e8"  # light blue
C_ACC = "#2ca02c"  # green
C_DAC = "#98df8a"  # light green
C_PTERM = "#ff7f0e"  # orange
C_COARSE_TRIM = "#d62728"  # red  — coarse trim steps
C_TC_ERROR = "#9467bd"  # purple — timer counter error
C_COARSE_ACC = "#c5b0d5"  # light purple — coarse accumulator
C_TEMP_OCXO = "#e377c2"  # pink
C_TEMP_BOARD = "#f7b6d2"  # light pink
C_WARMUP = "#ffeedd"
C_RUN = "#edfff0"
C_LOCK = "#d4efff"
C_VLINE = "#666666"

# ── Figure layout ─────────────────────────────────────────────────────────────

n_panels = 5 if has_temp else 4
fig_height = 17 if has_temp else 14
fig, axes = plt.subplots(n_panels, 1, figsize=(16, fig_height), sharex=True)
fig.suptitle(
    f"GPSDO PI-loop — {os.path.basename(args.logfile)}",
    fontsize=13, fontweight="bold", y=0.99
)
fig.subplots_adjust(hspace=0.08, top=0.96, bottom=0.05, left=0.08, right=0.97)

t_min, t_max = t[0], t[-1]


def shade_regions(ax):
    ax.axvspan(t_min, run_start_time or t_max, alpha=0.25, color=C_WARMUP, zorder=0)
    if run_start_time is not None:
        end_run = lock_time if lock_time else t_max
        ax.axvspan(run_start_time, end_run, alpha=0.25, color=C_RUN, zorder=0)
    if lock_time is not None:
        ax.axvspan(lock_time, t_max, alpha=0.25, color=C_LOCK, zorder=0)


def add_vlines(ax):
    if run_start_time is not None:
        ax.axvline(run_start_time, color=C_VLINE, lw=1.0, ls="--", zorder=3)
    if lock_time is not None:
        ax.axvline(lock_time, color="steelblue", lw=1.2, ls="-", zorder=3)


# ── Panel 1: Filtered + raw phase error ──────────────────────────────────────

ax1 = axes[0]
shade_regions(ax1)
add_vlines(ax1)
ax1.axhline(0, color="black", lw=0.5, zorder=2)
ax1.axhline(50, color=C_FILTERED, lw=0.6, ls=":", alpha=0.6, zorder=2)
ax1.axhline(-50, color=C_FILTERED, lw=0.6, ls=":", alpha=0.6, zorder=2)
ax1.axhline(100, color=C_FILTERED, lw=0.5, ls=":", alpha=0.35, zorder=2)
ax1.axhline(-100, color=C_FILTERED, lw=0.5, ls=":", alpha=0.35, zorder=2)
ax1.plot(t, raw_tic, color=C_RAW, lw=0.4, alpha=0.5, label="Raw TIC corrected net value (sawtooth)")
ax1.plot(t, filtered, color=C_FILTERED, lw=1.2, label="Filtered / EMA (I-term input)")
ax1.set_ylabel("Phase error\n(linearised TIC counts)", fontsize=9)
ax1.legend(loc="upper right", fontsize=8)
ax1.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax1.grid(True, which="major", alpha=0.3)
ax1.grid(True, which="minor", alpha=0.1)
ax1.text(t_max, 50, "  ±50 lock", va="center", fontsize=7, color=C_FILTERED, alpha=0.8)
ax1.text(t_max, -50, "  ±50 lock", va="center", fontsize=7, color=C_FILTERED, alpha=0.8)
ax1.text(t_max, 100, "  ±100 unlock", va="center", fontsize=7, color=C_FILTERED, alpha=0.5)
ax1.text(t_max, -100, "  ±100 unlock", va="center", fontsize=7, color=C_FILTERED, alpha=0.5)

# ── Panel 2: I Accumulator + DAC output ──────────────────────────────────────

ax2 = axes[1]
shade_regions(ax2)
add_vlines(ax2)
ax2_r = ax2.twinx()
ax2.plot(t, acc_v, color=C_ACC, lw=1.4, label="I Accumulator (EFC setpoint)")
ax2_r.plot(t, dac_v, color=C_DAC, lw=0.6, alpha=0.7, label="DAC output (I + P)")
ax2.set_ylabel("I Accumulator\n(V)", fontsize=9, color=C_ACC)
ax2_r.set_ylabel("DAC output\n(V)", fontsize=9, color=C_DAC)
ax2.tick_params(axis="y", labelcolor=C_ACC)
ax2_r.tick_params(axis="y", labelcolor=C_DAC)
lines2 = [mpatches.Patch(color=C_ACC, label="I Accumulator (EFC setpoint)"),
          mpatches.Patch(color=C_DAC, label="DAC output (I + P)")]
ax2.legend(handles=lines2, loc="upper right", fontsize=8)
ax2.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax2.grid(True, which="major", alpha=0.3)
ax2.grid(True, which="minor", alpha=0.1)

# ── Panel 3: P-term + Coarse Trim ────────────────────────────────────────────

ax3 = axes[2]
shade_regions(ax3)
add_vlines(ax3)
ax3.axhline(0, color="black", lw=0.5, zorder=2)
ax3.plot(t, pterm, color=C_PTERM, lw=0.7, alpha=0.85,
         label="P term (ticDelta × gain, clamped ±2000)")
ax3_r = ax3.twinx()
if coarse_trim_t:
    stem_container = ax3_r.stem(coarse_trim_t, coarse_trim_v,
                                linefmt=C_COARSE_TRIM, markerfmt="o", basefmt=" ",
                                label="Coarse trim (applied every coarseTrimPeriod s)")
    stem_container.markerline.set_color(C_COARSE_TRIM)
    stem_container.markerline.set_markersize(5)
    stem_container.stemlines.set_color(C_COARSE_TRIM)
ax3_r.axhline(0, color=C_COARSE_TRIM, lw=0.4, ls="--", alpha=0.4)
ax3.set_ylabel("P term\n(DAC counts)", fontsize=9, color=C_PTERM)
ax3_r.set_ylabel("Coarse trim\n(DAC counts)", fontsize=9, color=C_COARSE_TRIM)
ax3.tick_params(axis="y", labelcolor=C_PTERM)
ax3_r.tick_params(axis="y", labelcolor=C_COARSE_TRIM)
lines3 = [mpatches.Patch(color=C_PTERM, label="P term (ticDelta × gain, clamped ±2000)"),
          mpatches.Patch(color=C_COARSE_TRIM, label="Coarse trim applied (every coarseTrimPeriod s)")]
ax3.legend(handles=lines3, loc="upper right", fontsize=8)
ax3.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax3.grid(True, which="major", alpha=0.3)
ax3.grid(True, which="minor", alpha=0.1)

# ── Panel 4: Timer Counter Error + Coarse Error Accumulator ──────────────────

ax4 = axes[3]
shade_regions(ax4)
add_vlines(ax4)
ax4.axhline(0, color="black", lw=0.5, zorder=2)
ax4.plot(t, tc_error, color=C_TC_ERROR, lw=0.7, alpha=0.85,
         label="Timer Counter Error (coarse freq error, counts)")
ax4_r = ax4.twinx()
ax4_r.plot(t, coarse_accum, color=C_COARSE_ACC, lw=1.0, alpha=0.8,
           label="Coarse Error Accumulator (resets on trim)")
ax4.set_ylabel("Timer Counter\nError (counts)", fontsize=9, color=C_TC_ERROR)
ax4_r.set_ylabel("Coarse Error\nAccumulator", fontsize=9, color=C_COARSE_ACC)
ax4.tick_params(axis="y", labelcolor=C_TC_ERROR)
ax4_r.tick_params(axis="y", labelcolor=C_COARSE_ACC)
lines4 = [mpatches.Patch(color=C_TC_ERROR, label="Timer Counter Error (per tick)"),
          mpatches.Patch(color=C_COARSE_ACC, label="Coarse Error Accumulator")]
ax4.legend(handles=lines4, loc="upper right", fontsize=8)
ax4.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax4.grid(True, which="major", alpha=0.3)
ax4.grid(True, which="minor", alpha=0.1)

if not has_temp:
    ax4.set_xlabel("Time (s)", fontsize=10)

# ── Panel 5: Temperature (only if present in log) ────────────────────────────

if has_temp:
    ax5 = axes[4]
    shade_regions(ax5)
    add_vlines(ax5)
    t_ocxo_valid = [(t[i], temp_ocxo[i]) for i in range(len(t)) if temp_ocxo[i] is not None]
    t_board_valid = [(t[i], temp_board[i]) for i in range(len(t)) if temp_board[i] is not None]
    if t_ocxo_valid:
        ax5.plot([x[0] for x in t_ocxo_valid], [x[1] for x in t_ocxo_valid],
                 color=C_TEMP_OCXO, lw=1.0, label="OCXO temperature (°C)")
    if t_board_valid:
        ax5.plot([x[0] for x in t_board_valid], [x[1] for x in t_board_valid],
                 color=C_TEMP_BOARD, lw=1.0, label="Board temperature (°C)")
    ax5.set_ylabel("Temperature\n(°C)", fontsize=9)
    ax5.set_xlabel("Time (s)", fontsize=10)
    ax5.legend(loc="upper right", fontsize=8)
    ax5.yaxis.set_minor_locator(ticker.AutoMinorLocator())
    ax5.grid(True, which="major", alpha=0.3)
    ax5.grid(True, which="minor", alpha=0.1)

# ── Global annotations ────────────────────────────────────────────────────────

ax1.get_figure().canvas.draw()  # force layout so get_ylim() is valid
y_top = ax1.get_ylim()[1]
x_ann_off = (t_max - t_min) * 0.01

if run_start_time is not None:
    ax1.annotate(f"RUN\nT={run_start_time}",
                 xy=(run_start_time, y_top),
                 xytext=(run_start_time + x_ann_off, y_top * 0.90),
                 fontsize=7.5, color=C_VLINE,
                 arrowprops=dict(arrowstyle="-", color=C_VLINE, lw=0.8))

if lock_time is not None:
    ax1.annotate(f"LOCKED\nT={lock_time}",
                 xy=(lock_time, y_top),
                 xytext=(lock_time + x_ann_off, y_top * 0.90),
                 fontsize=7.5, color="steelblue",
                 arrowprops=dict(arrowstyle="-", color="steelblue", lw=0.8))

# Mode legend at the bottom
warmup_patch = mpatches.Patch(color=C_WARMUP, alpha=0.7, label="WARMUP mode")
run_patch = mpatches.Patch(color=C_RUN, alpha=0.7, label="RUN mode")
lock_patch = mpatches.Patch(color=C_LOCK, alpha=0.7, label="LOCKED")
fig.legend(handles=[warmup_patch, run_patch, lock_patch],
           loc="lower center", ncol=3, fontsize=8,
           bbox_to_anchor=(0.5, 0.005), framealpha=0.8)

# ── Output ────────────────────────────────────────────────────────────────────

if args.save:
    out_path = os.path.splitext(args.logfile)[0] + "_plot.png"
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
else:
    if args.f:
        figManager = plt.get_current_fig_manager()
        figManager.full_screen_toggle()  # try to open maximized for better visibility
    plt.show()
