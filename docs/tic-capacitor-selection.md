# TIC Capacitor Selection — C9

## Function

C9 is the timing capacitor in the Time-Interval Counter (TIC) circuit.
It is charged by a current source derived from the 10 MHz OCXO and
discharged on each GPS PPS edge. The ADC reading of the capacitor voltage
at the moment of discharge is the raw TIC value (0–1023 counts on a
10-bit ADC with a **1.1 V internal reference**), representing the
fractional phase offset between the PPS edge and the nearest OCXO
cycle — the core measurement the PI loop disciplines against.

1 ADC count ≈ **1 ns** of phase resolution
(1.1 V / 1024 counts ≈ 1.07 mV/count; full scale 1000 counts = 1000 ns = 1 µs).

Because the capacitor is the *entire* measurement element of the TIC,
its electrical characteristics directly set the noise floor, linearity,
and temperature stability of every phase-error sample.

---

## Original part — X7R MLCC 1 nF ±10%

The original BOM used a generic **X7R MLCC** (Class II ceramic).

### Problems with X7R for this application

| Property | X7R behaviour | Impact on TIC |
|---|---|---|
| **Voltage coefficient** | Capacitance drops 5–15% as voltage rises from 0 V to Vref | Charge curve is non-linear — the TIC output is not proportional to phase error. The cubic polynomial linearisation (`x2Coefficient`, `x3Coefficient` in `ControlState`) exists specifically to compensate for this. The coefficients were tuned at one voltage/temperature and are never perfectly correct. |
| **Temperature coefficient** | ±15% from −55 °C to +125 °C, non-linear and with hysteresis | `ticOffset` and the shape of the linearisation curve shift as the OCXO warms up and ambient temperature changes, causing a slowly-varying systematic error in every TIC measurement. |
| **Piezoelectric effect** | MLCCs generate charge when mechanically stressed | Acoustic noise or vibration injects spurious charge — effectively adding noise directly to the phase measurement. |
| **Tolerance** | ±10% | Minor concern compared to the above. |

The voltage coefficient effect is the dominant problem. It means the
TIC linearisation is an approximation that is only accurate at the
temperature and voltage conditions under which the coefficients were
measured. Between power cycles, or as the OCXO temperature stabilises,
the curve shifts and the loop is fighting a slowly-varying offset.

---

## Replacement part — Panasonic ECHU1H102GX5

**Panasonic ECHU series — PPS (polyphenylene sulphide) film, 1 nF, 50 V, ±2%, 0805**

Datasheet: https://www.lcsc.com/datasheet/C569826.pdf  
LCSC part: C569826

### Why PPS film is the right choice

| Property | PPS film (ECHU) | Improvement over X7R |
|---|---|---|
| **Voltage coefficient** | Essentially zero — capacitance is independent of applied voltage | The TIC charge curve is genuinely linear; `x2`/`x3` correction terms should be much smaller and more stable |
| **Temperature coefficient** | ~±50 ppm/°C, linear and repeatable | ~300× more stable than X7R; `ticOffset` will not drift with OCXO warmup |
| **Piezoelectric effect** | None (film dielectric, not ceramic) | No noise injection from vibration or acoustics |
| **Tolerance** | ±2% | Better run-to-run consistency of the TIC range |
| **Package** | 0805 SMD | Drop-in replacement for the existing C9 footprint |
| **Voltage rating** | 50 V | Comfortably above the 5 V reference |

PPS film is the preferred dielectric for precision timing and sampling
circuits. It is used in high-end sample-and-hold circuits for exactly
this reason — near-zero voltage coefficient means charge is strictly
proportional to time, not distorted by the voltage-dependent capacitance
of a ceramic dielectric.

### Expected firmware impact after swap

- **`x2Coefficient` and `x3Coefficient`** will likely be much smaller
  (curve closer to linear). After swapping, run in WARMUP-only mode,
  capture a full sawtooth cycle in the log, and re-fit the polynomial
  if the residual non-linearity is measurable. It may be negligible and
  both coefficients can be set to zero.
- **`ticOffset`** will remain near 500 (set by the RC time constants,
  which are unchanged) but will be more stable across temperature.
- **`TIC_MIN` / `TIC_MAX`** should remain approximately 12 / 1012 —
  the film cap has the same nominal capacitance, so the ADC range is
  unchanged.
- **Lock stability** should improve — the EMA filter will be chasing
  a quieter, more stable signal, so `ticCorrectedNetValueFiltered`
  should settle tighter than the current ±50 counts, and the sawtooth
  may compress further once the OCXO is on-frequency.

### What to verify on first run with the new cap

1. In a WARMUP-only run, confirm `TIC Value` still spans roughly
   12–1012 counts over a full sawtooth cycle.
2. Check that the sawtooth is visually more linear in the log (less
   curvature at the top and bottom of the ramp) compared to the X7R
   data.
3. If re-fitting the polynomial: set `x2Coefficient = 0` and
   `x3Coefficient = 0` first and see how much residual error remains
   in `ticCorrectedNetValue` — it may already be within acceptable
   bounds without any correction.

---

## C0G/NP0 as an alternative

If PPS film is not available, **C0G (NP0) MLCC** is the correct
ceramic alternative. C0G is a Class I dielectric with:

- Near-zero voltage coefficient (like film)
- Temperature coefficient ±30 ppm/°C (better than PPS)
- No significant piezoelectric effect

C0G is substantially better than X7R for this application and is
widely available in 0805 1 nF. It is the minimum recommended
specification for C9 on any future board revision.

**Do not use X5R — it has the same voltage coefficient problems as
X7R, just over a narrower temperature range.**

---

## Why 120 pF C0G is NOT a suitable substitute

120 pF C0G is an excellent dielectric choice but the **wrong value**.
The TIC circuit is a sawtooth integrator: a constant current source
charges C9 continuously, and the voltage at the PPS edge represents
the fractional phase offset. The charge current resistor is sized for
**1 nF** to give full ADC swing (0–5 V) over one full OCXO period.

With the same charge current and 120 pF instead of 1 nF:

```
V = I × t / C    (ADC Vref = 1.1 V, full-scale t = 1 µs for 1 nF)

Charge current sized for 1 nF:  I = C × Vref / t = 1 nF × 1.1 V / 1 µs = 1.1 µA

With 120 pF, same 1.1 µA current:
  Full-scale voltage reached after t = C × Vref / I = 120 pF × 1.1 V / 1.1 µA = 120 ns
  → ADC clips at full scale after only 120 ns of phase offset  (original = 1000 ns)
  → TIC range collapses to 12% of original
  → sawtooth wraps every ~0.7 s at 170 ppb offset  (original ~6 s)
  → almost no phase discrimination — loop is essentially blind
```

Using 120 pF **without changing the charge current resistor** would
make the TIC measurement useless. To use 120 pF correctly the charge
resistor would need to increase by **8.3×** — a hardware PCB change,
not a firmware fix.

**Order 1 nF** in C0G or PPS film. The ECHU1H102GX5 (PPS film, 1 nF,
0805) is the recommended part and is a true drop-in.

---

## Summary

| Part | Dielectric | Voltage coeff | Temp coeff | Piezo | Verdict |
|---|---|---|---|---|---|
| Original (X7R MLCC) | Class II ceramic | Poor (5–15%) | Poor (±15%) | Yes | ❌ Not recommended |
| **ECHU1H102GX5** | **PPS film** | **~0** | **±50 ppm/°C** | **No** | **✅ Recommended** |
| C0G/NP0 MLCC | Class I ceramic | ~0 | ±30 ppm/°C | Negligible | ✅ Good alternative |
| X5R MLCC | Class II ceramic | Poor | Moderate | Yes | ❌ Not recommended |




