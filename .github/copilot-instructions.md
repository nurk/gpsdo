# Copilot Instructions — GPSDO Firmware

Read this file first at the start of every session. Keep it up to date
when the codebase changes.

---

## Project overview

GPS-disciplined oscillator (GPSDO) firmware for an **ATmega4808**
microcontroller, written in C++ using the Arduino / PlatformIO framework.
The hardware uses a 10 MHz OCXO whose EFC (electronic frequency control)
voltage is set by an I²C 16-bit DAC (DAC8571). The 10 MHz output is divided
by 2 to produce a 5 MHz signal that is counted by TCA0 on the ATmega4808.
A GPS module supplies a 1 PPS signal. A Time-Interval Counter (TIC) circuit
measures the phase offset between the PPS edge and the OCXO.

Build: `platformio run`
Flash: `platformio run -t upload`
Board: ATmega4808, 28-pin-standard, 16 MHz internal oscillator
Debug output: Serial2 @ 115200 baud (hardware debug header)
GPS: Serial1 @ 9600 baud (u-blox, NMEA via TinyGPSPlus)

---

## Hard rules — never break these

- **Do not modify `archive/OriginalCode.cpp`**. It is a read-only
  reference for the original ATmega328p behaviour. Consult it only to
  understand legacy intent; do not port from it directly.
- **Do not use the original code as a template.** Tom wants his own clean
  implementation.
- **No magic numbers.** All hardware constants belong in `src/Constants.h`
  as named `constexpr` values.
- **Keep ISRs minimal.** ISRs only snapshot hardware registers and set
  flags. All calculation logic runs in the main loop via
  `CalculationController::calculate()`.
- **Use `ATOMIC_BLOCK(ATOMIC_RESTORESTATE)`** when reading any
  `volatile` ISR-shared variable from main context.
- **Always pet the watchdog** (`wdt_reset()`) in the main loop and in any
  long-running operation.
- **No dynamic memory allocation** (`new`, `malloc`, `String`). Use fixed
  buffers and stack variables only.
- **Clamp every DAC write** to `[0, DAC_MAX_VALUE]` before calling
  `setDac_()`. Never write an out-of-range value to the DAC.

---

## Source file map

| File | Purpose |
|---|---|
| `src/main.cpp` | Hardware init, ISRs, main loop, module wiring |
| `src/Constants.h` | All enums, constexpr constants, typedefs, `ControlState`, `GpsData` |
| `src/CalculationController.h/.cpp` | Measurement pipeline and (future) PI control loop |
| `src/LcdController.h/.cpp` | 20×4 HD44780 display, page rendering |
| `src/Callbacks.h` | Forward declarations for callback functions wired in main |
| `archive/OriginalCode.cpp` | Read-only legacy reference — do not edit |
| `docs/` | Architecture, todo list, migration notes, EEPROM layout |
| `logs/` | Serial capture logs from real hardware runs |

---

## Hardware constants (from `src/Constants.h`)

| Constant | Value | Meaning |
|---|---|---|
| `DAC_MAX_VALUE` | 65535 | 16-bit DAC full scale |
| `DAC_VREF` | 5.0 V | REF5050 voltage reference |
| `COUNTS_PER_PPS` | 5 000 000 | Expected TCA0 counts per GPS PPS (10 MHz OCXO ÷ 2 = 5 MHz counter) |
| `MODULO` | 50 000 | TCA0 counter period (overflows at 50 000) |
| `TIC_MIN` | 12.0 | Minimum valid raw TIC ADC count |
| `TIC_MAX` | 1012.0 | Maximum valid raw TIC ADC count |
| `WARMUP_TIME_DEFAULT` | 600 s | Default OCXO warm-up time |

---

## `ControlState` — current fields (in `src/Constants.h`)

```
dacValue                    uint16_t   — current DAC output value
dacVoltage                  float      — derived from dacValue / DAC_MAX_VALUE * DAC_VREF
holdValue                   int32_t    — DAC value to use in HOLD mode

timerCounterValueOld        int32_t    — TCA0 snapshot from previous PPS
timerCounterValueReal       int32_t    — TCA0 delta this PPS (should be ~0 for on-freq)
timerCounterError           int32_t    — COUNTS_PER_PPS - real - overflows*MODULO

time                        int32_t    — seconds since start
timeOld                     int32_t    — time at previous PPS

missedPpsCounter            int32_t    — cumulative missed PPS count
timeSinceMissedPps          int32_t    — seconds since last missed PPS

ticValue                    int32_t    — raw ADC reading this PPS
ticValueOld                 int32_t    — raw ADC reading last PPS
ticValueCorrection          double     — linearised TIC this PPS
ticValueCorrectionOld       double     — linearised TIC last PPS (snapshot)
ticValueCorrectionOffset    double     — linearise(ticOffset) — zero reference
ticCorrectedNetValue        double     — ticValueCorrection - ticValueCorrectionOffset (phase error)
ticCorrectedNetValueFiltered double    — EMA-filtered phase error (input to I-term)
ticFrequencyError           double     — ticValueCorrection - ticValueCorrectionOld (rate of phase change, ns/s)
ticFilterSeeded             bool       — false until first real value seeds the EMA
isFirstTic                  bool       — true until tick 1 has seeded all *Old snapshots; skips all calculations on tick 1
ticFilterConst              int32_t    — EMA time constant in seconds (default 16)

iAccumulator                double     — integrator state in DAC counts; initialised to mid-scale (32767)
iRemainder                  double     — fractional carry-forward for I-step to avoid truncation drift
timeConst                   int32_t    — loop time constant in seconds (default 32)
gain                        double     — DAC counts per linearised TIC count / EFC sensitivity (default 12.0)
damping                     double     — P/I ratio; higher = more damped, slower pull-in (default 3.0)

dacMinValue                 uint16_t   — lower DAC safety limit (default 0)
dacMaxValue                 uint16_t   — upper DAC safety limit (default 65535)

ppsLocked                   bool       — true when loop has been locked for ≥ 2×ticFilterConst consecutive seconds
ppsLockCount                int32_t    — consecutive seconds within LOCK_THRESHOLD; resets on any excursion

ticOffset                   double     — expected mid-point of TIC range (default 500.0)
x2Coefficient               double     — quadratic linearisation coeff (stored pre-scaled /1000)
x3Coefficient               double     — cubic linearisation coeff (stored pre-scaled /100000)
```

---

## `CalculationController` — pipeline (called once per PPS)

```
calculate()
  ├── timeKeeping()              — advance time, detect missed PPS
  ├── timerCounterNormalization() — compute timerCounterValueReal + timerCounterError
  ├── ticLinearization()         — cubic polynomial, produce ticCorrectedNetValue
  ├── ticPreFilter()             — EMA on ticCorrectedNetValue → ticCorrectedNetValueFiltered
  │                                Seeds on first tick; skips EMA until ticFilterSeeded == true
  ├── computeFrequencyError()    — ticFrequencyError = ticValueCorrection - ticValueCorrectionOld
  │                                Guarded by ticFilterSeeded (skipped on tick 2 before Old is valid)
  ├── piLoop(mode)               — PI control; only active when mode == RUN
  │                                P-term = ticFrequencyError * gain
  │                                I-step = ticCorrectedNetValueFiltered * gain / damping / timeConst
  │                                iAccumulator clamped to [dacMinValue, dacMaxValue]
  │                                dacOutput = iAccumulator + pTerm, clamped, written via setDac_()
  ├── lockDetection(mode)        — only active when mode == RUN
  │                                Counts consecutive seconds where abs(ticCorrectedNetValueFiltered) < LOCK_THRESHOLD (50)
  │                                Declares lock after lockCount ≥ 2 × ticFilterConst
  │                                Declares unlock immediately when abs > UNLOCK_THRESHOLD (100)
  │                                Resets ppsLocked and lockCount when leaving RUN mode
  │                                Drives LOCK_LED via ppsLocked (written in main loop)
  └── updateSnapshots()          — copy current values to *Old fields
```

The `mode` parameter (`RUN` / `HOLD` / `WARMUP`) gates both `piLoop()` and `lockDetection()` — both are no-ops unless `mode == RUN`.

---

## What has been validated

### log 2026-03-13-run2.log
- TIC sawtooth is clean, within TIC_MIN/MAX bounds. ✅
- TIC linearisation maths verified manually — `linearize(500) = 483.24`. ✅
- `ticCorrectedNetValue` correctly zero at TIC = 500 (ticOffset). ✅
- `timerCounterError` nominally −1 (expected for near-on-frequency OCXO). ✅
- DAC fixed at 29 000 / 2.2126 V throughout (no loop yet, WARMUP mode). ✅

### log 2026-03-13-run3.log
- EMA filter seeding confirmed: first tick filtered = raw (−493.66). ✅
- EMA arithmetic verified manually for multiple ticks — matches formula exactly. ✅
- Filter converges from −493 at Time 275 toward ~−70 by Time 614 (still converging,
  undisciplined OCXO, expected — full settle takes ~4 × filterConst = ~64 s more). ✅
- Mode transitions correctly: WARMUP (2) until Time 603, then RUN (0) at Time 604. ✅
- First-tick `timerCounterReal = 3667` and huge `timerCounterError` on Time 275 are
  expected boot artefacts (timerCounterValueOld = 0, overflowCount accumulated from
  power-on). Not a bug — consider skipping the first tick in future. ⚠️
- `ticFilterSeeded` flag works correctly — no premature EMA on tick 1. ✅
- Extended run to Time 1117 confirmed: filter oscillates around a stable mean of
  approximately −70 to −80 counts throughout RUN mode. The OCXO is free-running so
  it never converges to zero — this is the expected undisciplined baseline. ✅
- TIC sawtooth period is approximately 6–7 seconds (consistent ~170 count/s drift),
  implying the free-running OCXO is offset by roughly 170 ns/s ≈ 170 ppb. ✅
- No missed PPS events observed across the full 1117-second run. ✅
- Occasional `timerCounterReal` spikes of ±3 to ±8 continue to appear in balanced
  pairs (e.g. +8/−6 at T637/638, +7/−4 at T702/703) — GPS PPS jitter/latency,
  not a software bug. ✅

### log 2026-03-13-run4.log
- `ticFrequencyError` implementation verified: arithmetic confirmed as
  `ticValueCorrection[N] − ticValueCorrection[N−1]` for multiple ticks. ✅
  - T9→T10: `720.84 − 18.32 = 702.52` ✅
  - T10→T11: `497.50 − 720.84 = −223.34` ✅
  - T107→T108: `790.21 − 66.83 = 723.38` (logged 723.39, rounding only) ✅
- Guard clause (`ticFilterSeeded`) prevents premature computation on tick 2. ✅
- `ticFrequencyError` correctly alternates between large positive spikes (~700)
  at TIC sawtooth wrap-arounds and steady negative steps (~−165 to −242) within
  each sawtooth ramp — **this is expected behaviour** for an undisciplined OCXO.
  The spikes are real TIC phase resets, not software bugs.
- Steady-state drift between resets is approximately −170 to −200 counts/s,
  consistent with ~170–200 ppb free-running offset seen in run3. ✅
- DAC fixed at 29 000 / 2.2126 V (no loop, WARMUP mode throughout). ✅
- ⚠️ Note: `ticFrequencyError` is computed from the raw linearised correction
  (before offset subtraction), so wrap-around spikes will appear in the P-term
  unless explicitly handled when the PI loop is implemented.

---

## Next implementation steps (ordered)

These are documented in detail in `docs/path-to-disciplined-ocxo.md`.

### ~~Step 1 — TIC pre-filter~~ ✅ Done (validated in run3.log)

### ~~Step 2 — Frequency error (`ticFrequencyError`)~~ ✅ Done (validated in run4.log)

### ~~Step 3 — PI control loop~~ ✅ Done (awaiting validation run)
- Added to `ControlState`: `iAccumulator` (double, init mid-scale), `iRemainder` (double),
  `timeConst` (int32_t, default 32), `gain` (double, default 12.0),
  `damping` (double, default 3.0), `dacMinValue` / `dacMaxValue` (uint16_t, 0 / 65535).
- Private method `piLoop(OpMode mode)` implemented in `CalculationController`.
- Only executes when `mode == RUN`.
- P-term = `ticFrequencyError * gain`; I-step = `ticCorrectedNetValueFiltered * gain / damping / timeConst`.
- Fractional I-step carried forward in `iRemainder` to avoid truncation drift.
- `iAccumulator` clamped to `[dacMinValue, dacMaxValue]` to prevent wind-up.
- Final `dacOutput = iAccumulator + pTerm`, clamped and written via `setDac_()`.

### Step 4 — DAC clamp / safety limits ✅ Done (implemented alongside Step 3)
- `dacMinValue` / `dacMaxValue` added to `ControlState` (defaults 0 / 65535).
- Every DAC write in `piLoop()` is clamped to these limits.

### ~~Step 5 — Lock detection~~ ✅ Done (awaiting validation run)
- Added to `ControlState`: `ppsLocked` (bool), `lockCount` (int32_t).
- Added to `Constants.h`: `LOCK_THRESHOLD` (50.0), `UNLOCK_THRESHOLD` (100.0).
- Private method `lockDetection(OpMode mode)` implemented in `CalculationController`.
- Resets `ppsLocked` / `lockCount` when mode is not `RUN`.
- Requires `lockCount ≥ 2 × ticFilterConst` consecutive seconds below `LOCK_THRESHOLD` to declare lock.
- Unlocks immediately when `abs(ticCorrectedNetValueFiltered) > UNLOCK_THRESHOLD`.
- `LOCK_LED` is driven from `ppsLocked` in the main loop after each PPS event.

### Step 6 — Validate and tune
- Flash firmware and run with loop closed (mode transitions to RUN after warmup).
- Watch `iAccumulator` in the log: it should drift slowly toward the correct DAC value.
- Watch `ticCorrectedNetValue` sawtooth compress over time.
- Watch for "LOCKED" message in the debug log after convergence.
- If the loop oscillates: increase `damping` or `timeConst`.
- If the loop is too slow to pull in: decrease `timeConst` or increase `gain`.

---

## Coding conventions

- Use `int32_t`, `uint16_t` etc. — not bare `int` or `long`.
- Use `constexpr` for all compile-time constants; add them to `Constants.h`.
- Use `static_cast<>()` rather than C-style casts.
- Avoid `double / int` without an explicit cast — always cast the divisor
  to `double` to avoid silent integer division.
- New private methods in `CalculationController` must be declared in
  `CalculationController.h`.
- All `DEBUG_*` serial output must be inside `#ifdef DEBUG_*` guards.
  The `DEBUG_CALCULATION` flag is enabled in `platformio.ini`.
- After every edit run the linter mentally: no narrowing conversions,
  no integer division in float context, no uninitialised state reads.

---

## Keep this file up to date

After every session, update:
- **`ControlState` fields** if new fields were added or changed.
- **Pipeline section** if new methods were added to `CalculationController`.
- **Validated section** if a new log confirmed correct behaviour.
- **Next steps** if a step was completed or a new one was identified.



