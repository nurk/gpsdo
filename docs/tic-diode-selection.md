# TIC Diode Selection — D2

## Role of D2 in the TIC circuit

In this TIC topology, D2 is placed **in series before C9**, between the
constant current source and the capacitor. The current source (derived
from the 10 MHz OCXO) flows **forward through D2** to charge C9. On each
GPS PPS edge a reset switch (transistor) discharges C9 back to near ground,
restarting the sawtooth. The ADC samples the capacitor voltage just before
each reset.

D2 in this position has two jobs:

1. **Isolation / blocking** — D2 prevents the reset switch from discharging
   back through the current source path during the reset pulse. It decouples
   the charge and discharge paths.
2. **Series voltage drop** — Because D2 is forward-biased continuously during
   charging, its forward voltage V_F appears as a voltage offset in the
   charge path. This does **not** affect the sawtooth shape directly (the
   current source still drives a constant dV/dt into C9), but V_F must be
   stable — any drift in V_F with temperature causes the effective charge
   current to change, shifting `TIC_MAX` and the sawtooth baseline.

> **Note — what is NOT a concern in this topology:**
> Reverse leakage through D2 is irrelevant here. D2 is forward-biased
> during charging and there is no period where it is reverse-biased with
> significant voltage across it. The 1N5817's 1 mA reverse leakage (the
> dominant problem in a discharge-clamp topology) does **not** apply.

---

## Current part — 1N5817 Schottky

The 1N5817 is a **1 A Schottky rectifier**.

| Parameter | 1N5817 typical |
|---|---|
| Forward voltage V_F | ~0.30 V @ 1 mA, ~0.45 V @ 1 A |
| V_F temperature coefficient | ~ −1.5 mV/°C (typical Schottky) |
| Junction capacitance C_J | ~100–200 pF |
| Reverse recovery time t_rr | < 10 ns (Schottky — no minority carrier storage) |
| Package | DO-41 through-hole or SMA SMD |

The 1N5817 was chosen for its low forward voltage (~0.30 V at the ~1 µA
charge current), which maximises the voltage headroom available to charge
C9 toward the 1.1 V ADC reference. At such a low bias current the actual
V_F is closer to **0.20–0.25 V**, so the capacitor charges to roughly
0.85–0.90 V maximum, consistent with the observed `TIC_MAX` ≈ 1012 counts
(≈ 0.89 × 1024 × 1.1 V).

### Impact on TIC in this topology

The 1N5817's large junction capacitance (~150 pF) is a concern:

- C9 is nominally 1 nF. The 150 pF junction capacitance of the 1N5817 is
  **~15% of C9**. The diode capacitance is effectively in parallel with the
  charge path and partially with C9, depending on the circuit layout.
- As C9 charges and the voltage across the diode changes slightly, C_J
  varies (junction capacitance is voltage-dependent for Schottky diodes),
  introducing a small non-linearity into the charge ramp.
- This is a contributing factor to the observed need for quadratic and cubic
  linearisation coefficients (`x2Coefficient`, `x3Coefficient`).

The forward voltage temperature coefficient (~−1.5 mV/°C) shifts V_F
by ~30 mV over a 20 °C temperature swing, which alters the effective charge
headroom and causes a small drift in `TIC_MAX`. This is a secondary
source of the `ticOffset` drift observed during OCXO warmup.

---

## Proposed replacement — 1N4148W

The 1N4148W is a **small-signal fast switching diode**.

| Parameter | 1N4148W typical |
|---|---|
| Forward voltage V_F | ~0.60–0.72 V @ 1 mA; **~0.45–0.50 V @ 1 µA** |
| V_F temperature coefficient | ~ −2.0 mV/°C (silicon p-n junction) |
| Junction capacitance C_J | ~4 pF |
| Reverse recovery time t_rr | ≤ 4 ns |
| Package | SOD-123 (1N4148W) — SMD drop-in |

At the ~1 µA charge current the 1N4148W's V_F is approximately **0.45–0.50 V**
(the 0.60–0.72 V datasheet figure is at 1–10 mA). The Schottky 1N5817 at
the same current is ~0.20–0.25 V. The 1N4148W therefore claims
~0.25 V more of the 1.1 V headroom.

---

## Comparison

| Property | 1N5817 | 1N4148W | Impact |
|---|---|---|---|
| **Forward voltage @ 1 µA** | ~0.20–0.25 V | ~0.45–0.50 V | 1N4148W reduces TIC_MAX headroom by ~230 mV |
| **V_F temperature coefficient** | ~−1.5 mV/°C | ~−2.0 mV/°C | 1N4148W has slightly more V_F drift with temperature |
| **Junction capacitance C_J** | ~150 pF | **~4 pF** | 1N4148W is 37× lower — much less non-linearity |
| **Reverse leakage** | ~1 mA | ~5 nA | Irrelevant in this topology (D2 is forward-biased) |
| **Reverse recovery** | < 10 ns | ≤ 4 ns | Both fast — not a concern |

---

## Effect of higher forward voltage (V_F)

Because D2 is in the charge path, a higher V_F means the capacitor
charges to a lower peak voltage, reducing `TIC_MAX`.

### Estimated TIC_MAX with 1N4148W

Current situation (1N5817, V_F ≈ 0.22 V @ 1 µA):
```
V_C9_max ≈ 1.1 V − 0.22 V = 0.88 V
TIC_MAX ≈ 0.88 / 1.1 × 1024 ≈ 819 counts
```
Observed TIC_MAX = 1012 counts ≈ 0.89 V — consistent (the ADC reference
headroom is slightly looser than the simple subtraction implies).

With 1N4148W (V_F ≈ 0.47 V @ 1 µA):
```
V_C9_max ≈ 1.1 V − 0.47 V = 0.63 V
TIC_MAX ≈ 0.63 / 1.1 × 1024 ≈ 587 counts
```

**`TIC_MAX` will decrease significantly — from ~1012 to approximately
550–620 counts.** The sawtooth will have a lower ceiling.

Consequences:
- `TIC_MAX` in `Constants.h` must be updated after the swap.
- `ticOffset` must be updated to the new midpoint
  (~`(TIC_MIN + TIC_MAX) / 2`).
- The loop gain (`gain`) may need minor retuning because the linearised
  TIC range has shrunk — 1 raw count now represents a slightly different
  phase offset in ns.

### Effect on TIC_MIN

`TIC_MIN` is set by how completely C9 discharges during the reset pulse,
not by the diode forward voltage. The reset switch shorts C9 directly.
`TIC_MIN` will remain approximately 12 counts after the swap.

---

## Effect on non-linearity

The 1N4148W's 4 pF junction capacitance vs the 1N5817's ~150 pF is the
**dominant improvement** in this topology. The voltage-dependent variation
of the large Schottky junction capacitance adds a non-linear term to the
charge ramp. With C_J = 4 pF (< 0.5% of C9 = 1 nF), this effect is
negligible.

**Expected result:** the quadratic (`x2Coefficient`) and cubic
(`x3Coefficient`) linearisation coefficients may be reducible
significantly — potentially to zero. Verify on first run.

---

## Effect on loop constants

The loop constants (`gain`, `damping`, `timeConst`, `ticFilterConst`)
are set by the loop dynamics. Most are unchanged, but because `TIC_MAX`
decreases, the phase-to-count mapping changes slightly:

- **`gain`** — may need minor adjustment after updating `TIC_MAX` and
  `ticOffset`. The gain is in DAC counts per linearised TIC count; if the
  linearised range is smaller, the same 1 ns of phase error maps to fewer
  counts, requiring a proportionally higher gain.
- **`damping`, `timeConst`, `ticFilterConst`** — unchanged. These are
  control loop time constants, not hardware parameters.
- **`ticOffset`** — **must be updated** to `(TIC_MIN + TIC_MAX) / 2` for
  the new diode.
- **`TIC_MAX`** — **must be updated** in `Constants.h` after observing
  the new sawtooth ceiling.

---

## Summary of required `Constants.h` changes after swap

| Constant | Current value | Expected after 1N4148W swap | Action |
|---|---|---|---|
| `TIC_MAX` | 1012.0 | ~550–620 | **Update after first run** |
| `ticOffset` | 500.0 | ~`(12 + new_TIC_MAX) / 2` | **Update after first run** |
| `x2Coefficient` | 1.0e-4 | Likely smaller or zero | Re-measure or zero and observe |
| `x3Coefficient` | 3.0e-7 | Likely smaller or zero | Re-measure or zero and observe |
| `gain` | 12.0 | Possibly slightly higher | Adjust if loop is sluggish after swap |

---

## What to verify on first run with the new diode

1. Run in WARMUP-only mode. Observe `TIC Value` sawtooth and record the
   new `TIC_MAX` ceiling.
2. Update `TIC_MAX` and `ticOffset` in `Constants.h` to match.
3. Set `x2Coefficient = 0` and `x3Coefficient = 0`. Observe
   `ticCorrectedNetValue` for residual non-linearity. If the ramp is
   visually linear, leave them at zero.
4. If `gain` feels sluggish after the swap, increase it proportionally
   to the reduction in TIC range:
   `new_gain = old_gain × (old_TIC_range / new_TIC_range)`
5. Let the loop run to convergence and confirm lock is achieved within
   a similar timeframe to previous runs.

---

## Verdict

| Property | 1N5817 | 1N4148W | Verdict |
|---|---|---|---|
| Forward voltage @ 1 µA | ✅ Low (~0.22 V) | ⚠ Higher (~0.47 V) | 1N4148W reduces TIC_MAX — requires Constants.h update |
| Junction capacitance | ❌ High (~150 pF) | ✅ Low (~4 pF) | **Major improvement** — less linearisation needed |
| V_F temperature stability | ⚠ −1.5 mV/°C | ⚠ −2.0 mV/°C | Marginally worse, but less significant than C_J improvement |
| Reverse leakage | (irrelevant in this topology) | (irrelevant) | — |
| Loop retuning required | — | **TIC_MAX + ticOffset must be updated** | Minor — measure on first run |

**The 1N4148W is still an improvement for D2 in this topology**, primarily
because its 4 pF junction capacitance is 37× lower than the 1N5817's ~150 pF,
reducing non-linearity in the charge ramp and allowing the linearisation
polynomial to be simplified or eliminated.

The tradeoff is a lower `TIC_MAX` (~580 vs ~1012 counts) due to the higher
V_F at µA-level currents. This requires updating `TIC_MAX` and `ticOffset`
in `Constants.h` after the first run. The loop constants themselves do not
need significant retuning, but `gain` may need a small upward adjustment
to compensate for the reduced TIC range.

The 1N5817 is not a bad choice for this topology (its low V_F actually helps
maximise TIC range), but its large junction capacitance is the primary source
of charge-ramp non-linearity.
