#!/usr/bin/env python3
"""
plot_loop.py — GPSDO PI-loop visualiser
Usage:  python3 utils/plot_loop.py logs/<logfile>.log
        python3 utils/plot_loop.py logs/<logfile>.log --save  # save PNG instead of showing

Plots (all vs time in seconds):
  1. TIC Corrected Net Value Filtered  — EMA phase error driving the I-term
  2. TIC Corrected Net Value (raw)     — per-tick phase, shows the sawtooth
  3. I Accumulator                     — integrator state (long-term EFC setpoint)
  4. DAC Value                         — actual EFC output (I + P combined)
  5. P term                            — proportional correction each tick
  6. TIC delta                         — rate of change of phase (P-term input)

Vertical annotations:
  • WARMUP → RUN transition
  • LOCKED declaration

Usage:
# Show interactive window
python3 utils/plot_loop.py logs/2026-03-14-run8.log

# Save a PNG next to the log file
python3 utils/plot_loop.py logs/2026-03-14-run8.log --save

"""

import sys
import os
import re
import argparse
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.ticker as ticker

# ── Argument parsing ──────────────────────────────────────────────────────────

parser = argparse.ArgumentParser(description="Plot GPSDO loop data from a serial log file.")
parser.add_argument("logfile", help="Path to the .log file")
parser.add_argument("--save", action="store_true",
                    help="Save PNG next to the log file instead of opening a window")
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
        # The LOCKED message appears on the line BEFORE the tick that declared it
        nxt = parse_line(lines[i + 1].rstrip())
        if nxt and lock_time is None:
            lock_time = int(nxt["Time"])
        i += 1
        continue
    d = parse_line(raw)
    if d:
        mode = int(d.get("Mode", 2))
        t    = int(d["Time"])
        if prev_mode == 2 and mode == 0 and run_start_time is None:
            run_start_time = t
        prev_mode = mode
        records.append({
            "t":        t,
            "mode":     mode,
            "filtered": float(d.get("TIC Corrected Net Value Filtered", 0)),
            "raw":      float(d.get("TIC Corrected Net Value", 0)),
            "acc":      float(d.get("I Accumulator", 0)),
            "dac":      int(d.get("DAC Value", 0)),
            "pterm":    float(d.get("P term", 0)),
            "delta":    float(d.get("TIC delta", 0)),
            "lockcount":int(d.get("PPS Lock count", 0)),
        })
    i += 1

if not records:
    print("No data records found — check the log file format.", file=sys.stderr)
    sys.exit(1)

# ── Unpack into lists ─────────────────────────────────────────────────────────

t         = [r["t"]         for r in records]
filtered  = [r["filtered"]  for r in records]
raw_tic   = [r["raw"]       for r in records]
acc       = [r["acc"]       for r in records]
dac       = [r["dac"]       for r in records]
pterm     = [r["pterm"]     for r in records]
delta     = [r["delta"]     for r in records]
mode      = [r["mode"]      for r in records]
lockcount = [r["lockcount"] for r in records]

# DAC to voltage
dac_v = [d / 65535 * 5.0 for d in dac]
acc_v = [a / 65535 * 5.0 for a in acc]

# ── Colour scheme ─────────────────────────────────────────────────────────────

C_FILTERED = "#1f77b4"   # blue
C_RAW      = "#aec7e8"   # light blue
C_ACC      = "#2ca02c"   # green
C_DAC      = "#98df8a"   # light green
C_PTERM    = "#ff7f0e"   # orange
C_DELTA    = "#ffbb78"   # light orange
C_WARMUP   = "#ffeedd"   # warm background
C_RUN      = "#edfff0"   # cool background
C_LOCK     = "#d4efff"   # locked background
C_VLINE    = "#666666"   # annotation lines

# ── Figure layout ─────────────────────────────────────────────────────────────

fig, axes = plt.subplots(4, 1, figsize=(16, 14), sharex=True)
fig.suptitle(
    f"GPSDO PI-loop — {os.path.basename(args.logfile)}",
    fontsize=13, fontweight="bold", y=0.98
)
fig.subplots_adjust(hspace=0.08, top=0.95, bottom=0.06, left=0.08, right=0.97)

t_min, t_max = t[0], t[-1]

def shade_regions(ax):
    """Background colour bands for WARMUP / RUN / LOCKED regions."""
    # WARMUP
    ax.axvspan(t_min, run_start_time or t_max, alpha=0.25, color=C_WARMUP, zorder=0, label="_warmup")
    # RUN (before lock)
    if run_start_time is not None:
        end_run = lock_time if lock_time else t_max
        ax.axvspan(run_start_time, end_run, alpha=0.25, color=C_RUN, zorder=0, label="_run")
    # LOCKED
    if lock_time is not None:
        ax.axvspan(lock_time, t_max, alpha=0.25, color=C_LOCK, zorder=0, label="_locked")

def add_vlines(ax):
    """Annotate mode transition and lock declaration."""
    if run_start_time is not None:
        ax.axvline(run_start_time, color=C_VLINE, lw=1.0, ls="--", zorder=3)
    if lock_time is not None:
        ax.axvline(lock_time, color="steelblue", lw=1.2, ls="-", zorder=3)

# ── Panel 1: Filtered phase error (main PI-loop signal) ──────────────────────

ax1 = axes[0]
shade_regions(ax1)
add_vlines(ax1)
ax1.axhline(0, color="black", lw=0.5, zorder=2)
ax1.axhline(50,  color=C_FILTERED, lw=0.6, ls=":", alpha=0.6, zorder=2)
ax1.axhline(-50, color=C_FILTERED, lw=0.6, ls=":", alpha=0.6, zorder=2)
ax1.plot(t, raw_tic,  color=C_RAW,      lw=0.4, alpha=0.5, label="Raw TIC corrected net value")
ax1.plot(t, filtered, color=C_FILTERED, lw=1.2,             label="Filtered (EMA, I-term input)")
ax1.set_ylabel("Phase error\n(linearised TIC counts)", fontsize=9)
ax1.legend(loc="upper right", fontsize=8)
ax1.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax1.grid(True, which="major", alpha=0.3)
ax1.grid(True, which="minor", alpha=0.1)

# Annotate lock threshold lines
ax1.text(t_max, 50,  "  lock threshold (+50)", va="center", fontsize=7, color=C_FILTERED, alpha=0.8)
ax1.text(t_max, -50, "  lock threshold (−50)", va="center", fontsize=7, color=C_FILTERED, alpha=0.8)

# ── Panel 2: I Accumulator and DAC output (EFC voltage) ──────────────────────

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

lines2  = [mpatches.Patch(color=C_ACC, label="I Accumulator (EFC setpoint)"),
           mpatches.Patch(color=C_DAC, label="DAC output (I + P)")]
ax2.legend(handles=lines2, loc="upper right", fontsize=8)
ax2.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax2.grid(True, which="major", alpha=0.3)
ax2.grid(True, which="minor", alpha=0.1)

# ── Panel 3: P-term ───────────────────────────────────────────────────────────

ax3 = axes[2]
shade_regions(ax3)
add_vlines(ax3)
ax3.axhline(0, color="black", lw=0.5, zorder=2)
ax3.plot(t, pterm, color=C_PTERM, lw=0.7, alpha=0.85, label="P term (ticDelta × gain, clamped ±2000)")
ax3.set_ylabel("P term\n(DAC counts)", fontsize=9)
ax3.legend(loc="upper right", fontsize=8)
ax3.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax3.grid(True, which="major", alpha=0.3)
ax3.grid(True, which="minor", alpha=0.1)

# ── Panel 4: TIC delta ────────────────────────────────────────────────────────

ax4 = axes[3]
shade_regions(ax4)
add_vlines(ax4)
ax4.axhline(0, color="black", lw=0.5, zorder=2)
ax4.plot(t, delta, color=C_DELTA, lw=0.6, alpha=0.8, label="TIC delta (phase rate-of-change, P-term source)")
ax4.set_ylabel("TIC delta\n(counts/s)", fontsize=9)
ax4.set_xlabel("Time (s)", fontsize=10)
ax4.legend(loc="upper right", fontsize=8)
ax4.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax4.grid(True, which="major", alpha=0.3)
ax4.grid(True, which="minor", alpha=0.1)

# ── Global annotations ────────────────────────────────────────────────────────

# Annotate transition markers on the top panel only
y_top = ax1.get_ylim()[1]

if run_start_time is not None:
    ax1.annotate(f"RUN\nT={run_start_time}",
                 xy=(run_start_time, y_top), xytext=(run_start_time + (t_max - t_min) * 0.01, y_top * 0.92),
                 fontsize=7.5, color=C_VLINE,
                 arrowprops=dict(arrowstyle="-", color=C_VLINE, lw=0.8))

if lock_time is not None:
    ax1.annotate(f"LOCKED\nT={lock_time}",
                 xy=(lock_time, y_top), xytext=(lock_time + (t_max - t_min) * 0.01, y_top * 0.92),
                 fontsize=7.5, color="steelblue",
                 arrowprops=dict(arrowstyle="-", color="steelblue", lw=0.8))

# Background legend
warmup_patch = mpatches.Patch(color=C_WARMUP, alpha=0.7, label="WARMUP mode")
run_patch    = mpatches.Patch(color=C_RUN,    alpha=0.7, label="RUN mode")
lock_patch   = mpatches.Patch(color=C_LOCK,   alpha=0.7, label="LOCKED")
fig.legend(handles=[warmup_patch, run_patch, lock_patch],
           loc="lower center", ncol=3, fontsize=8,
           bbox_to_anchor=(0.5, 0.01), framealpha=0.8)

# ── Output ────────────────────────────────────────────────────────────────────

if args.save:
    out_path = os.path.splitext(args.logfile)[0] + "_plot.png"
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved: {out_path}")
else:
    plt.show()

