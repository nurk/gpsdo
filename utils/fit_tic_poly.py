#!/usr/bin/env python3
"""
fit_tic_poly.py — Refit TIC linearisation polynomial from a WARMUP log.

The firmware linearises raw TIC ADC counts with a normalised Horner cubic:

    s  = (tic - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000
    x1 = 1 - x2*1000 - x3*100000          (unity-gain constraint)
    linearized(tic) = s * (x1 + s * (x2 + s * x3))

where x2 = x2Coefficient and x3 = x3Coefficient as stored in ControlState.

The log records (TIC Value, TIC Correction) pairs from WARMUP ticks.
We fit new x2/x3 to best reproduce those corrections, compare options:
  A  current defaults
  B  refitted cubic  (x2 + x3 free)
  C  refitted quadratic (x3 = 0)
  D  no correction   (x2 = x3 = 0)

using step-variance as the linearity metric.

Usage:
    python3 utils/fit_tic_poly.py logs/2026-04-10-run1.log [--plot]
"""

import re
import sys
import numpy as np

TIC_MIN = 12.0
TIC_MAX = 1012.0

def fw_linearize(tic, x2, x3):
    """Exact replica of the firmware ticLinearization() computation."""
    x1 = 1.0 - x2 * 1000.0 - x3 * 100000.0
    s  = (tic - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0
    return s * (x1 + s * (x2 + s * x3))

PATTERN = re.compile(
    r'TIC Value: (\d+), TIC Correction: ([\-\d.]+).*?'
    r'TIC Correction Offset: ([\-\d.]+).*?Mode: (\d+)'
)

def parse_log(path):
    raw_list, corr_list = [], []
    tic_offset = None
    with open(path) as f:
        for line in f:
            m = PATTERN.search(line)
            if not m or int(m.group(4)) != 2:
                continue
            raw = int(m.group(1))
            if raw < TIC_MIN or raw > TIC_MAX:
                continue
            raw_list.append(raw)
            corr_list.append(float(m.group(2)))
            if tic_offset is None:
                tic_offset = float(m.group(3))
    return np.array(raw_list, dtype=float), np.array(corr_list, dtype=float), tic_offset

def step_variance(raw_seq, x2, x3):
    corrected = fw_linearize(raw_seq, x2, x3)
    diffs = np.diff(corrected)
    mask = np.abs(diffs) < 300
    if mask.sum() < 5:
        return float('nan')
    return float(np.std(diffs[mask]))

def fit_cubic(raw, corr):
    """
    Solve fw_linearize(raw, x2, x3) = corr  in least-squares sense.

    fw_linearize(tic, x2, x3)
      = s * (1 - 1000*x2 - 100000*x3  +  s*(x2 + s*x3))
      = s  +  x2*(s^2 - 1000*s)  +  x3*(s^3 - 100000*s)

    corr - s = x2*(s^2 - 1000*s) + x3*(s^3 - 100000*s)
    """
    s = (raw - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0
    A = np.column_stack([s**2 - 1000.0*s, s**3 - 100000.0*s])
    rhs = corr - s
    x2, x3 = np.linalg.lstsq(A, rhs, rcond=None)[0]
    return float(x2), float(x3)

def fit_quadratic(raw, corr):
    s = (raw - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0
    A = (s**2 - 1000.0*s).reshape(-1, 1)
    rhs = corr - s
    x2 = float(np.linalg.lstsq(A, rhs, rcond=None)[0][0])
    return x2, 0.0

def fit_rms(raw, corr, x2, x3):
    return float(np.sqrt(np.mean((fw_linearize(raw, x2, x3) - corr)**2)))

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <logfile> [--plot]")
        sys.exit(1)

    raw, corr, tic_offset = parse_log(sys.argv[1])
    print(f"Parsed {len(raw)} WARMUP ticks  (ticOffset={tic_offset})")
    if len(raw) < 20:
        print("ERROR: Too few WARMUP ticks.")
        sys.exit(1)

    cur_x2, cur_x3 = 1.0e-4, 3.0e-7

    # Sanity: at raw=500 the log always shows TIC Correction ~ fw_linearize(500)
    chk = fw_linearize(500.0, cur_x2, cur_x3)
    print(f"  Sanity: fw_linearize(500) with current coeffs = {chk:.4f}")
    print(f"  (log T370 shows TIC Correction = 483.24 at raw=500, ticOffset=483.24)")
    print(f"  (net of offset that is 0.0 — expected) ✓\n")

    new_x2, new_x3 = fit_cubic(raw, corr)
    q_x2,   q_x3   = fit_quadratic(raw, corr)

    options = [
        ("A current",   cur_x2, cur_x3),
        ("B cubic fit", new_x2, new_x3),
        ("C quad fit",  q_x2,   q_x3),
        ("D no corr",   0.0,    0.0),
    ]

    print(f"{'Option':<16} {'x2':>14} {'x3':>14}  {'RMS':>8}  {'step-std':>10}")
    print("-" * 70)
    results = {}
    for name, x2, x3 in options:
        rms = fit_rms(raw, corr, x2, x3)
        sv  = step_variance(raw, x2, x3)
        print(f"{name:<16} {x2:>14.4e} {x3:>14.4e}  {rms:>8.3f}  {sv:>10.4f}")
        results[name] = (x2, x3, rms, sv)

    print("\n── Sample correction table (TIC Value → fw_linearize output) ──")
    print(f"{'Raw':>6}  {'[A]':>10}  {'[B]':>10}  {'[C]':>10}  {'[D]':>10}")
    for v in [12, 100, 200, 300, 400, tic_offset, 500, 600, 700, 800, 900, 1000, 1012]:
        a = fw_linearize(v, cur_x2, cur_x3)
        b = fw_linearize(v, new_x2, new_x3)
        c = fw_linearize(v, q_x2,  q_x3)
        d = fw_linearize(v, 0.0,   0.0)
        print(f"{v:>6.1f}  {a:>10.3f}  {b:>10.3f}  {c:>10.3f}  {d:>10.3f}")

    # Pick best by step-variance (lower = more linear, ignoring nan)
    finite = {k: v for k, v in results.items() if not np.isnan(v[3])}
    if finite:
        best = min(finite, key=lambda k: finite[k][3])
        bx2, bx3 = finite[best][0], finite[best][1]
        print(f"\n── Recommendation ──")
        print(f"  Best option: [{best}]  (step-std = {finite[best][3]:.4f})")
    else:
        best, bx2, bx3 = "B cubic fit", new_x2, new_x3
        print("\n── Recommendation (step-variance unavailable, using refitted cubic) ──")

    print(f"\n  x2Coefficient = {bx2:.6e}")
    print(f"  x3Coefficient = {bx3:.6e}")
    print("\n── Paste into src/Constants.h ──────────────────────────────────────")
    print(f"    double x2Coefficient = {bx2:.6e}; // quadratic term")
    print(f"    double x3Coefficient = {bx3:.6e}; // cubic term")

    if '--plot' in sys.argv:
        try:
            import matplotlib.pyplot as plt
            fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

            r = np.linspace(TIC_MIN, TIC_MAX, 600)
            ax1.scatter(raw, corr, s=2, alpha=0.3, color='grey', label='Log data')
            ax1.plot(r, fw_linearize(r, cur_x2, cur_x3), 'r-', lw=1.5, label=f'[A] current')
            ax1.plot(r, fw_linearize(r, new_x2, new_x3), 'b-', lw=1.5, label=f'[B] refitted')
            ax1.plot(r, fw_linearize(r, q_x2,  q_x3),  'g--', lw=1.5, label=f'[C] quadratic')
            ax1.plot(r, r, 'k:', lw=1, label='identity')
            ax1.set_xlabel('Raw TIC ADC'); ax1.set_ylabel('Linearised value')
            ax1.set_title('TIC linearisation'); ax1.legend(fontsize=8)

            res = corr - fw_linearize(raw, new_x2, new_x3)
            ax2.scatter(raw, res, s=2, alpha=0.4, color='blue')
            ax2.axhline(0, color='k', lw=0.5)
            ax2.set_xlabel('Raw TIC ADC'); ax2.set_ylabel('Residual (log − refitted)')
            ax2.set_title(f'Refitted cubic residuals  RMS={fit_rms(raw,corr,new_x2,new_x3):.2f}')

            plt.tight_layout(); plt.show()
        except ImportError:
            print("(matplotlib not available — skipping plot)")

if __name__ == '__main__':
    main()

