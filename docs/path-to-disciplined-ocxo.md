# Path to a Disciplined OCXO

This document describes the ordered steps required to go from the current
state (measurement pipeline working, no control output) to a fully
GPS-disciplined OCXO.

---

## Current state (baseline)

- TIC value is captured by the ADC on every PPS edge.
- The raw TIC is linearised by a cubic polynomial into `ticValueCorrection`.
- The result is zero-centred around `ticOffset` into `ticCorrectedNetValue`.
- The DAC is set to a fixed value — there is no feedback loop yet.
- The log confirms the measurement pipeline is correct and the sawtooth
  pattern of the free-running OCXO is clearly visible.

---

## Step 1 — TIC pre-filter (EMA low-pass on phase error)

### Why

The raw `ticCorrectedNetValue` jumps by ±50–150 counts cycle-to-cycle.
Feeding this noise directly into a controller would stir the integrator on
every tick and produce a jittery DAC output.
An exponential moving average (EMA) smooths the phase error before it
reaches the controller.

### What to implement

Add to `ControlState`:

- `double ticFiltered` — the EMA output (units: linearised TIC counts,
  same as `ticCorrectedNetValue`).
- `int32_t filterConst` — the filter time constant in seconds (tunable,
  start with 16).

Add a private method `ticPreFilter()` to `CalculationController`, called
from `calculate()` after `ticLinearization()`:

```
ticFiltered += (ticCorrectedNetValue - ticFiltered) / filterConst
```

The filter constant should be reduced to 1 (no filtering) when not locked,
so the loop can pull in quickly, then increased once lock is achieved.

### Validation

Log `ticFiltered` alongside the raw `ticCorrectedNetValue`.
You should see the filtered value track the sawtooth envelope slowly and
smoothly, with the cycle-to-cycle noise removed.

---

## Step 2 — Frequency error term (`diff`)

### Why

A PI loop needs two independent signals:

| Signal | Meaning | Used for |
|---|---|---|
| Filtered phase error | Where the PPS edge is | I-term (slow, steady-state correction) |
| Frequency error | How fast the phase is drifting | P-term (fast damping) |

Without the frequency error the P-term would respond to phase position
rather than phase velocity, which gives a sluggish or oscillatory response.

### What to implement

`ticValueCorrectionOld` is already snapshotted in `updateSnapshots()`.
Add to `ControlState`:

- `double ticFrequencyError` — the per-second change in linearised TIC.

Compute it in a new private method `computeFrequencyError()` (or inline
in the pre-filter step):

```
ticFrequencyError = ticValueCorrection - ticValueCorrectionOld
```

Units: linearised TIC counts per second ≈ nanoseconds/second ≈ parts per
billion (ppb), which is a natural unit for an OCXO tuning voltage.

### Validation

Log `ticFrequencyError`. On an undisciplined OCXO it will be a small
near-constant value (the free-running frequency offset in ppb).
This number directly tells you how far off the OCXO is before you close
the loop.

---

## Step 3 — PI control loop

### Why PI and not PID

- **P-term** on the frequency error provides fast damping of frequency
  excursions without integrating noise.
- **I-term** on the filtered phase error integrates any remaining phase
  offset to zero over time.
- **D-term is not needed.** The OCXO is already a stable, low-noise
  integrator. A D-term would differentiate GPS timing jitter directly into
  the DAC and destabilise the loop.

### What to implement

Add to `ControlState`:

- `double iAccumulator` — the integrator state (in DAC counts, scaled by
  `timeConst` to keep fractional precision).
- `double iRemainder` — fractional part of the integrator step, carried
  forward each second to avoid truncation drift.
- `int32_t timeConst` — loop time constant in seconds (tunable, start
  with 32).
- `double gain` — DAC counts per linearised TIC count (tunable, start
  with 12). Represents the OCXO EFC sensitivity.
- `double damping` — P/I ratio (tunable, start with 3.0).

Add a private method `piControl()` to `CalculationController`, only
executed when `mode == RUN` and after warmup:

```
P_term = ticFrequencyError * gain
I_step = P_term / damping / timeConst   (+= iRemainder)
iAccumulator += floor(I_step)
iRemainder    = fractional part of I_step

dacOutput = (iAccumulator / timeConst) + P_term
```

Clamp `dacOutput` to `[0, DAC_MAX_VALUE]` before writing.
Clamp `iAccumulator` correspondingly to prevent integrator wind-up.

### Key tuning parameters (starting points)

| Parameter | Starting value | Effect |
|---|---|---|
| `timeConst` | 32 s | Longer = slower, more stable |
| `gain` | 12 | Higher = more aggressive, may oscillate |
| `damping` | 3.0 | Higher = more damped, slower pull-in |
| `filterConst` | 16 s | Longer = smoother phase signal to I-term |

### Validation

With the loop closed, the sawtooth in `ticCorrectedNetValue` should
progressively compress. The `iAccumulator` should drift slowly toward
the correct DAC value for your OCXO's free-running offset.
Lock is achieved when `abs(ticFiltered)` stays below a threshold
(e.g. ±50 counts) for several `timeConst` periods.

---

## Step 4 — DAC clamp and safety limits (Task 5)

Before or alongside Step 3, add configurable min/max DAC limits and clamp
the output before every `setDac_()` call. This is already listed as
Task 5 in `docs/todo-list.md` and should be done at the same time as
closing the loop.

---

## Recommended implementation order

```
Step 1: ticPreFilter()         → validate filtered signal in log
Step 2: computeFrequencyError() → validate ppb reading in log
Step 3: piControl()            → close the loop, observe pull-in
Step 4: DAC clamp              → safety, implement alongside Step 3
```

Each step produces a loggable output that can be validated independently
before moving to the next one.

