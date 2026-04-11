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
- **Do not use the original code as a template.** He wants his own clean
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
| `src/Constants.h` | All enums, constexpr constants, typedefs, `ControlState`, `GpsData`, `EEPROMState` |
| `src/CalculationController.h/.cpp` | Measurement pipeline and PI control loop |
| `src/ExternalEEPROMController.h/.cpp` | 24LC128 wear-levelled EEPROM persistence (8 rotating banks) |
| `src/LcdController.h/.cpp` | 20×4 HD44780 display, page rendering |
| `src/Callbacks.h` | Forward declarations for callback functions wired in main |
| `archive/OriginalCode.cpp` | Read-only legacy reference — do not edit |
| `docs/` | Architecture, todo list, migration notes, EEPROM layout |
| `docs/tic-capacitor-selection.md` | TIC capacitor (C9) analysis — X7R problems, ECHU1H102GX5 (PPS film) recommendation, C0G alternative |
| `docs/tic-diode-selection.md` | TIC diode (D2) analysis — D2 is in series **before** C9, forward-biased during charging. 1N5817 junction capacitance (~150 pF) causes charge-ramp non-linearity. 1N4148W (4 pF C_J) recommended — reduces TIC_MAX due to higher V_F (~0.47 V vs ~0.22 V), requires updating TIC_MAX + ticOffset after swap. Reverse leakage is irrelevant in this topology. |
| `logs/` | Serial capture logs from real hardware runs |
| `utils/fit_tic_poly.py` | TIC polynomial re-fitter — parses WARMUP ticks from a log, fits new x2/x3 using the exact Horner-form firmware model, compares current / refitted cubic / quadratic / zero options by step-variance metric. Run: `python3 utils/fit_tic_poly.py <logfile> [--plot]` |

---

## Hardware constants (from `src/Constants.h`)

| Constant | Value | Meaning |
|---|---|---|
| `DAC_MAX_VALUE` | 65535 | 16-bit DAC full scale |
| `DAC_VREF` | 5.0 V | REF5050 voltage reference for the DAC |
| `ADC_VREF` | 1.1 V | ATmega4808 internal reference for the TIC ADC |
| `COUNTS_PER_PPS` | 5 000 000 | Expected TCA0 counts per GPS PPS (10 MHz OCXO ÷ 2 = 5 MHz counter) |
| `MODULO` | 50 000 | TCA0 counter period (overflows at 50 000) |
| `TIC_MIN` | 12.0 | Minimum valid raw TIC ADC count |
| `TIC_MAX` | 1012.0 | Maximum valid raw TIC ADC count |
| `WARMUP_TIME_DEFAULT` | 600 s | Default OCXO warm-up time |
| `DAC_INITIAL_VALUE` | 22880 | Best-guess EFC starting point on cold boot (midpoint of run9/run10 settled values) |
| `LOCK_THRESHOLD` | 80.0 | Lock declared when `abs(filtered)` stays below this for ticFilterConst consecutive s |
| `UNLOCK_THRESHOLD` | 200.0 | Lock lost immediately when `abs(filtered)` exceeds this (single tick) |
| `PTERM_MAX_COUNTS` | 2000.0 | Maximum absolute DAC counts the P-term may contribute per tick (non-wrap ticks only) |
| `TIC_WRAP_THRESHOLD` | 400.0 | `\|ticDelta\|` above this is a TIC sawtooth wrap — P-term zeroed for that tick |

---

## `ControlState` — current fields (in `src/Constants.h`)

```
isFirstTic                  bool       — true until tick 1 has seeded all *Old snapshots; skips all calculations on tick 1

dacValue                    uint16_t   — current DAC output value; initialised from DAC_INITIAL_VALUE
dacVoltage                  float      — derived from dacValue / DAC_MAX_VALUE * DAC_VREF
holdValue                   int32_t    — DAC value to use in HOLD mode

timerCounterValueOld        int32_t    — TCA0 snapshot from previous PPS
timerCounterValueReal       int32_t    — TCA0 delta this PPS (should be ~0 for on-freq)
timerCounterError           int32_t    — COUNTS_PER_PPS - real - overflows*MODULO

time                        int32_t    — seconds since start
timeOld                     int32_t    — time at previous PPS
storeStateTime              int32_t    — seconds elapsed in RUN mode; incremented only when opMode == RUN;
                                         drives the three-phase EEPROM save cadence in storeState()

missedPpsCounter            int32_t    — cumulative missed PPS count
timeSinceMissedPps          int32_t    — seconds since last missed PPS

ticValue                    int32_t    — raw ADC reading this PPS
ticValueOld                 int32_t    — raw ADC reading last PPS
ticValueCorrection          double     — linearised TIC this PPS
ticValueCorrectionOld       double     — linearised TIC last PPS (snapshot)
ticValueCorrectionOffset    double     — linearise(ticOffset) — zero reference
ticCorrectedNetValue        double     — ticValueCorrection - ticValueCorrectionOffset (phase error)
ticCorrectedNetValueFiltered double    — EMA-filtered phase error (input to I-term)
ticFrequencyError           double     — combined frequency error: ticDelta + timerCounterError×200
  │                                        ticDelta = ticCorrectedNetValue − (ticValueCorrectionOld − ticValueCorrectionOffset)
  │                                        timerCounterError×200 converts coarse counter ticks (200 ns each) to ns units
ticDelta                    double     — rate of change of offset-subtracted phase error (P-term source)
ticFilterSeeded             bool       — false until first real value seeds the EMA
ticFilterConst              int32_t    — EMA time constant in seconds (default 16)

iAccumulator                double     — integrator state in DAC counts; seeded from DAC_INITIAL_VALUE (or EEPROM on warm boot)
iRemainder                  double     — fractional carry-forward for I-step to avoid truncation drift; always 0.0 on boot
timeConst                   int32_t    — loop time constant in seconds (default 32)
gain                        double     — DAC counts per linearised TIC count / EFC sensitivity (default 12.0)
damping                     double     — P/I ratio; higher = more damped, slower pull-in (default 3.0)
pTerm                       double     — proportional term (DAC counts) — logged each PPS

coarseErrorAccumulator      double     — running sum of timerCounterError; reset each coarseTrimPeriod
coarseTrimGain              double     — DAC counts applied per accumulated timerCounterError count (default 0.5)
coarseTrimPeriod            int32_t    — seconds between coarse trim steps (default 64; must be > timeConst)
lastCoarseTrim              double     — most recent coarse trim value applied (0 on non-trim ticks; logged each PPS)
iTermSuppressCount          int32_t    — countdown: I-term is suppressed (step skipped, iRemainder zeroed) while > 0;
                                         set to coarseTrimPeriod when a coarse trim fires away from a rail, giving
                                         the trim a full period to work before the I-term can drain it back

dacMinValue                 uint16_t   — lower DAC safety limit (default 0)
dacMaxValue                 uint16_t   — upper DAC safety limit (default 65535)

ppsLocked                   bool       — true when loop has been locked for ≥ ticFilterConst consecutive seconds
ppsLockCount                int32_t    — consecutive seconds within LOCK_THRESHOLD; resets on any excursion

ticOffset                   double     — equilibrium TIC value; the loop drives TIC to this at lock.
                                         **Must be set to the midpoint of the actual hardware TIC sawtooth range** to
                                         remove the I-term's systematic pull-in bias during large frequency errors.
                                         With C0G cap + 1N4148W diode, observed sawtooth spans TIC_MIN (~12) to
                                         ~780–815 hardware counts → midpoint ~395–413. **Default 400.0**
                                         (was 500.0, which assumed TIC_MAX=1012 — incorrect for this hardware).
                                         Why it matters: when ticOffset ≠ sawtooth midpoint, the I-term has a
                                         persistent DC bias equal to (sawtooth_mean − ticOffset) / ticFilterConst
                                         per tick, which can drive the loop in the wrong direction during pull-in
                                         from a warm-start seed that requires the DAC to move upward.
x2Coefficient               double     — quadratic linearisation coeff (stored pre-scaled /1000; **default 0.0 for C0G cap, was 1.0e-4 for X7R**)
x3Coefficient               double     — cubic linearisation coeff (stored pre-scaled /100000; **default 0.0 for C0G cap, was 3.0e-7 for X7R**)
```

---

## `EEPROMState` — persisted fields (in `src/Constants.h`)

```
dacValue                    uint16_t   — last settled DAC output value
iAccumulator                double     — integrator state (DAC counts)
```

`EEPROMState` is a plain aggregate (no default member initializers). There is no
`isValid` field — warm/cold boot distinction is tracked by `ExternalEEPROMController::isValid_`
(private) and exposed via `isValid()` (public accessor). The in-memory `isValid` sentinel
that existed in earlier versions has been removed.

A compile-time cold-start fallback is defined in `Constants.h`:
```cpp
constexpr EEPROMState DEFAULT_EEPROM_STATE = {
    DAC_INITIAL_VALUE,  // dacValue
    DAC_INITIAL_VALUE,  // iAccumulator — cold-start seed, matches dacValue
};
```
`loadState()` returns `DEFAULT_EEPROM_STATE` when no valid EEPROM bank is found.

**Rules:**
- `iRemainder` is **never** stored — it is a fractional carry-forward meaningful only within a
  continuous run. It is always initialised to 0.0 on boot.
- `dacVoltage` is **never** stored — it is always derived from `dacValue` on load.
- Bump the `kMagic` version byte in `ExternalEEPROMController.h` (`0x47505301` → `0x47505302`
  etc.) whenever `EEPROMState`'s stored layout changes. This causes stale banks to be rejected
  on the next boot.

---

## `ExternalEEPROMController` — EEPROM wear-levelling (in `src/ExternalEEPROMController.h/.cpp`)

- **Hardware:** 24LC128 (128 Kbit = 16 KB) at I²C address 0x50.
- **Bank layout:** 8 banks × 2048 bytes. Each bank holds an 8-byte header
  (4-byte magic + 4-byte sequence number) followed by the payload
  (`dacValue` + `iAccumulator` = 10 bytes). Banks rotate on every save
  (round-robin) to spread wear evenly.
- **Magic:** `0x47505301` ("GPS" + version byte `0x01`). Bump the version byte
  whenever `EEPROMState` stored layout changes.
- **Write order:** payload written before header. If power is lost mid-write the
  old header is still intact, so the previous bank remains valid on next boot.
- **`begin()`:** scans all banks, finds the highest valid sequence number (`>`
  comparison — deterministic on tie). Sets `isValid_` and `activeBank_`.
- **`isValid()`:** public accessor — returns `isValid_`. Used in `setup()` to log
  warm vs cold boot.
- **`loadState()`:** if `isValid_`, reads the active bank and deserialises fields
  explicitly (no blind `memcpy`). If not valid, returns `DEFAULT_EEPROM_STATE`.
  `EEPROMState` no longer contains an `isValid` field — use `isValid()` instead.
- **`saveState()`:** serialises fields explicitly, writes payload then header to
  the next bank, updates `activeBank_` and `activeSeq_`.
- **`invalidate()`:** wipes magic + seq on all banks (used via serial 'i' command).
- **Serial commands (in `processCommands()`):**
  - `'s'` — manually trigger a save.
  - `'i'` — invalidate all banks (forces cold-start next boot).

---

## `CalculationController` — pipeline (called once per PPS)

```
calculate()
  ├── timeKeeping()              — advance time, detect missed PPS
  ├── timerCounterNormalization() — compute timerCounterValueReal + timerCounterError
  ├── ticLinearization()         — cubic polynomial, produce ticCorrectedNetValue
  ├── ticPreFilter()             — EMA on ticCorrectedNetValue → ticCorrectedNetValueFiltered
  │                                Seeds on first tick; sets ticFilterSeeded = true
  ├── computeFrequencyError()    — ticDelta = ticCorrectedNetValue - (ticValueCorrectionOld - ticValueCorrectionOffset)
  │                                ticFrequencyError = ticDelta + timerCounterError × 200 (ns per 5 MHz counter tick)
  │                                Both stored in ControlState.
  │                                No ticFilterSeeded guard needed — ticPreFilter() always sets it first.
  ├── piLoop(mode)               — PI control; only active when mode == RUN
  │                                P-term = ticDelta * gain (NOT ticFrequencyError — coarse term caused GPS jitter noise)
  │                                  Wrap suppression: if |ticDelta| > TIC_WRAP_THRESHOLD (400), P-term is zeroed for
  │                                  that tick. A TIC sawtooth wrap produces |ticDelta| ~700–800 in the wrong direction
  │                                  (negative spike when OCXO slow, positive when fast) — opposite of the correct
  │                                  correction. Zeroing for one tick is harmless; the I-term continues uninterrupted.
  │                                  On non-wrap ticks, clamped to ±PTERM_MAX_COUNTS.
  │                                I-step = ticCorrectedNetValueFiltered * gain / damping / timeConst
  │                                  + iRemainder (fractional carry-forward)
  │                                iAccumulator updated with floor(iStep); remainder stored in iRemainder.
  │                                Overshoot guard (conditional integration): if ticCorrectedNetValue and
  │                                  ticCorrectedNetValueFiltered have opposite signs, the EMA filter is
  │                                  lagging behind a zero-crossing of the raw phase error — the OCXO has
  │                                  already crossed the setpoint but the filter memory still reflects the
  │                                  old large error. In this case the I-step is skipped and iRemainder is
  │                                  zeroed. This prevents the filter lag from driving the integrator past
  │                                  the true setpoint (the overshoot seen in run2).
  │                                  Guard uses TWO consecutive raw samples (current and previous tick):
  │                                  both must be opposite to the filtered value to trigger. A single-tick
  │                                  sign opposite fires every ~4 ticks on the normal sawtooth (proven to
  │                                  block the I-term during pull-in — fixed 2026-04-10).
  │                                Anti-windup: I-step discarded (and iRemainder zeroed) if accumulator is
  │                                  already at a rail and step would push further into it.
  │                                iAccumulator clamped to [dacMinValue, dacMaxValue].
  │                                Coarse trim (outer loop on timerCounterError):
  │                                  coarseErrorAccumulator += timerCounterError each tick (guarded by COARSE_ERROR_SANITY_LIMIT).
  │                                  Every coarseTrimPeriod seconds: iAccumulator += coarseErrorAccumulator * coarseTrimGain,
  │                                  coarseErrorAccumulator reset to 0, lastCoarseTrim recorded.
  │                                  Anti-windup: trim discarded if already at a rail and trim would push further in.
  │                                  Rail-recovery suppression: if trim fires away from a rail (trimAway), iTermSuppressCount
  │                                    is set to coarseTrimPeriod and iRemainder is zeroed — I-term is suspended for that
  │                                    many ticks so the coarse trim has a full period to work before the I-term can drain it.
  │                                  Corrects residual frequency offsets the fine TIC loop cannot null.
  │                                dacOutput = iAccumulator + pTerm, clamped, written via setDac_()
  ├── lockDetection(mode)        — only active when mode == RUN
  │                                Counts consecutive seconds where abs(ticCorrectedNetValueFiltered) < LOCK_THRESHOLD (50)
  │                                Declares lock after ppsLockCount ≥ ticFilterConst (one full EMA time constant)
  │                                Declares unlock immediately when abs(filtered) > UNLOCK_THRESHOLD (200)
  │                                Resets ppsLocked and ppsLockCount when leaving RUN mode
  │                                Drives LOCK_LED via ppsLocked (written in main loop)
  │                                No iDrift guard — phase condition alone is sufficient (see run7/run8 notes)
  ├── storeState()               — periodic EEPROM save via saveState_ callback
  │                                No-op when opMode != RUN (no saves during WARMUP or HOLD).
  │                                state_.storeStateTime incremented each RUN tick; drives save cadence.
  │                                Phase 1 (storeStateTime < 3600 s):   save every 10 minutes
  │                                Phase 2 (storeStateTime < 43200 s):  save every hour
  │                                Phase 3 (storeStateTime ≥ 43200 s):  save every 12 hours
  │                                Uses storeStateTime (not state_.time) — phases crossed exactly once
  │                                regardless of how long WARMUP ran.
  └── updateSnapshots()          — copy current values to *Old fields
```

The `mode` parameter (`RUN` / `HOLD` / `WARMUP`) gates both `piLoop()` and `lockDetection()` — both are no-ops unless `mode == RUN`.

### Boot-time EEPROM seeding (in `setup()`)
```
externalEepromController.begin()   — scan all banks, find highest valid seq
loadState()                        — returns DEFAULT_EEPROM_STATE (cold) or EEPROM values (warm)
setEEPROMState()                   — always called unconditionally:
                                     restores dacValue + iAccumulator; derives dacVoltage;
                                     zeroes iRemainder; calls setDac_() immediately.
                                     On cold boot DEFAULT_EEPROM_STATE seeds both fields from
                                     DAC_INITIAL_VALUE, so behaviour is identical to old cold-start path.
isValid()                          — checked after setEEPROMState() for log message only:
                                     "Warm boot: EEPROM state loaded" or
                                     "Cold boot: no valid EEPROM bank — using defaults"
```

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

### log 2026-03-14-run1.log
- First run with PI loop closed (mode transitions WARMUP→RUN at T604). ✅
- During WARMUP (T69–T603): DAC fixed at 32767 / 2.5000 V, iAccumulator frozen at 32767.5 — correct. ✅
- **Bug identified**: P-term was overwhelming the output — DAC swinging between ~22000 and ~40000 every 2–3 ticks (≈1.4 V range). Root cause: raw `ticFrequencyError` of ±400–600 counts × gain 12 = ±5000–7000 DAC counts per tick. ⚠️
- **Fix 1**: `computeFrequencyError()` now uses `ticCorrectedNetValue - (ticValueCorrectionOld - ticValueCorrectionOffset)` — the rate of change of the offset-subtracted phase error. ✅
- **Fix 2**: P-term clamped to `±PTERM_MAX_COUNTS` (2000 counts) before being added to iAccumulator. The I-term handles long-term correction; the P-term only provides damping. ✅
- iAccumulator was drifting slowly downward (~9 counts/tick) in RUN mode, confirming the I-term is working, but was overwhelmed by the P-term. ✅
- lockCount briefly reached 1 on a few ticks but immediately reset — expected with an undisciplined OCXO and swinging DAC. ✅

### log 2026-03-14-run2.log
- Extended run (1728 seconds). Mode transitions WARMUP→RUN at T604. ✅
- P-term clamp confirmed working: `ticFrequencyError` of ±500 × gain 12 = ±6000 is correctly clamped to ±2000. DAC alternates between `iAccumulator ± 2000` every tick — expected for free-running OCXO sawtooth. ✅
- DAC swing is now ±2000 counts (≈0.15 V) instead of ±7000+ counts — a major improvement. ✅
- `iAccumulator` drifting downward at ~10–11 counts/tick during pull-in, confirming I-term is working. At T680, accumulator reached 31899 — correctly heading toward the settled setpoint. ✅
- `ticFrequencyError` values now in ±250–660 range during pull-in (no more ±800 spikes from the old raw computation). ✅
- **LOCKED declared at T1179** (lockCount = 32 = 2 × ticFilterConst). ✅
- `iAccumulator` stabilised at **~24250–24300** (≈1.85 V) — the true EFC setpoint for this OCXO at ambient temperature. ✅
- After lock, `iAccumulator` continues to drift slowly downward (~5 counts/tick), reaching ~23550 by T1728. This slow residual drift indicates the OCXO is still not perfectly on-frequency at this DAC value — the I-term is still correcting. Expected; will converge further over a longer run. ✅
- At lock, the TIC sawtooth still spans ~0–800 counts with a ~13-tick period. Each wrap produces a 1-tick P-term spike of ±2000 counts (DAC plunges to ~22100–22300 for one tick), but the `iAccumulator` itself is stable. This is correct P-term behaviour — not loop instability. ✅
- `ticCorrectedNetValueFiltered` after lock oscillates between about ±50 counts and is centred near zero — confirming the loop is correctly nulling the phase error. ✅
- `ticFrequencyError` on normal (non-wrap) ticks reduced to ±50–100 counts — down from ±600 at loop open. OCXO drift rate decreasing as the I-term homes in. ✅
- lockCount holds at exactly 32 continuously from T1179 onward (never drops to 0 again after lock) — lock detection is stable. ✅
- No missed PPS events across the full 1728-second run. ✅
- **Remaining item to watch:** slow residual iAccumulator drift post-lock (~5 counts/tick). At this rate the integrator will reach a new stable point in another ~200–300 seconds. The TIC sawtooth will compress further once the OCXO is closer to on-frequency.

### ~~Step 3 — PI control loop~~ ✅ Done (validated in run2/run5/run6/run8/run9)
- Added to `ControlState`: `iAccumulator` (double, init mid-scale), `iRemainder` (double),
  `timeConst` (int32_t, default 32), `gain` (double, default 12.0),
  `damping` (double, default 3.0), `dacMinValue` / `dacMaxValue` (uint16_t, 0 / 65535).
- Private method `piLoop(OpMode mode)` implemented in `CalculationController`.
- Only executes when `mode == RUN`.
- P-term = `ticFrequencyError * gain`; I-step = `ticCorrectedNetValueFiltered * gain / damping / timeConst`.
- Fractional I-step carried forward in `iRemainder` to avoid truncation drift.
- `iAccumulator` clamped to `[dacMinValue, dacMaxValue]` to prevent wind-up.
- Final `dacOutput = iAccumulator + pTerm`, clamped and written via `setDac_()`.

### ~~Step 4 — DAC clamp / safety limits~~ ✅ Done (implemented alongside Step 3)
- `dacMinValue` / `dacMaxValue` added to `ControlState` (defaults 0 / 65535).
- Every DAC write in `piLoop()` is clamped to these limits.

### ~~Step 5 — Lock detection~~ ✅ Done (validated in run8/run9)
- Added to `ControlState`: `ppsLocked` (bool), `lockCount` (int32_t).
- Added to `Constants.h`: `LOCK_THRESHOLD` (50.0), `UNLOCK_THRESHOLD` (100.0).
- Private method `lockDetection(OpMode mode)` implemented in `CalculationController`.
- Resets `ppsLocked` / `lockCount` when mode is not `RUN`.
- Requires `lockCount ≥ 2 × ticFilterConst` consecutive seconds below `LOCK_THRESHOLD` to declare lock.
- Unlocks immediately when `abs(ticCorrectedNetValueFiltered) > UNLOCK_THRESHOLD`.
- `LOCK_LED` is driven from `ppsLocked` in the main loop after each PPS event.

### log 2026-03-14-run3.log
- **Extended run — 3831 seconds total.** Mode transitions WARMUP→RUN at T604. ✅
- **No LOCKED declaration in the entire run** — the integrator drift guard is working correctly. ✅
- iAccumulator at T604: 32757. At T3823: ~21880. Total drift: ~10,880 counts over 3,219 s = ~3.4 counts/tick continuously downward. ✅ (loop is working; OCXO has not yet reached its true setpoint at this temperature)
- **Key difference from run2:** The OCXO's true on-frequency EFC setpoint for this run's thermal conditions is much lower (~19000–20000 counts ≈ 1.45–1.52 V) vs run2 (~24250 ≈ 1.85 V). This implies significantly warmer ambient temperature or OCXO not yet thermally stable when RUN mode engaged at T604.
- **TIC sawtooth never compressed** — still full-range (~0–800 counts, ~4-tick period) at T3823. `ticFrequencyError` on non-wrap ticks is ~150–200 counts throughout, indicating the OCXO is running ~150–200 ppb below the target frequency even at DAC ~21900. The loop is still pulling in.
- `ticCorrectedNetValueFiltered` oscillates around ±10–20 counts near zero from ~T1100 onward. This produces brief lockCount increments (1–2) but never reaches 32, because the integrator drift guard correctly identifies ongoing I-term motion (~3–4 counts/tick). ✅
- **The loop algorithm is correct.** The run simply needs more time. At 3.4 counts/tick, full convergence is expected around T5500–T7000 total (another 2000–4000 s beyond the end of this log).
- No missed PPS events across the full 3831-second run. ✅
- **Root cause of slow convergence:** Almost certainly thermal — OCXO was not at its stable operating temperature when WARMUP ended at T604. The EFC setpoint drifted significantly lower as the crystal warmed further.
- **Action required:** Implement EEPROM seeding (Step 8) to avoid long re-convergence on restart. Also consider extending `WARMUP_TIME_DEFAULT` or adding a temperature-stability check before transitioning to RUN.
- ⚠️ **The P-term clamp (±2000) hits on every sawtooth wrap (every 4–5 ticks).** Because wraps produce balanced ±2000 spikes, average P contribution is near zero. The I-term is doing all the work. This is correct behaviour for an OCXO far from setpoint.

### log 2026-03-14-run3-2.log
- **Continuation of run3** — SSH dropped and reconnected. Log picks up at T5516 (T3832 visible as partial first line). **The firmware continued running uninterrupted during the ~1684-second gap.** ✅
- **iAccumulator at T5516: ~8271** — down from ~21880 at T3831. Drop of ~13,600 counts over ~1685 seconds = **~8 counts/tick**. The drift rate more than doubled compared to run3.
- **Reason for accelerated drift:** As the iAccumulator fell below the true EFC setpoint and the DAC voltage dropped below the on-frequency point, the OCXO began running *faster* than the GPS. The `ticCorrectedNetValueFiltered` became more negative (~-30 to -50 counts), increasing the I-step to ~5–8 counts/tick.
- **iAccumulator still falling at T5739: ~7728** — but the drift rate is already slowing: T5516–T5739 shows only ~2.4 counts/tick (down from ~8 counts/tick in the gap). The loop is crossing the on-frequency point and decelerating. Convergence is expected ~400–800 seconds beyond T5739 (i.e., around T6200–T6500 total).
- `timerCounterReal` values are consistently **-5 to -7** (vs -1 to +3 in run3), confirming the OCXO is now running *fast* — the loop has overshot past the EFC setpoint. `timerCounterError` = +5 to +7 counts.
- TIC sawtooth still full-range, ~4-tick period. `ticFrequencyError` on non-wrap ticks is ~90–140 counts (slightly lower than run3's 150–200 — consistent with OCXO being closer to frequency but on the wrong side).
- **No lock declared.** lockCount reaches 1–2 on filtered zero-crossings but always resets. ✅
- **Conclusion: The OCXO's true on-frequency EFC setpoint at this run's temperature is somewhere below ~7700 counts (≈0.59 V)**. This is an extremely low EFC voltage for this OCXO — implies either very high ambient temperature, or the OCXO characteristics have changed significantly between run2 and run3. Run longer to observe final convergence.
- ⚠️ **The EFC setpoint variation between run2 (~24250 counts, 1.85 V) and this run (heading toward <8000 counts, <0.61 V) is ~16,000 counts (~1.2 V).** This is an enormous range for a single OCXO and suggests severe thermal variation between runs. EEPROM seeding becomes critical to avoid multi-hour convergence on each restart.

### log 2026-03-14-run4.log
- **Full run T158–T9113 (8955 seconds total).** Mode transition WARMUP→RUN at T605. ✅
- WARMUP: DAC fixed at 32767 / 2.5000 V, `iAccumulator` frozen at 32767.5, P term = 0.000 throughout. ✅
- TIC sawtooth during WARMUP: full range ~0–810 counts, ~4-tick period (~200 ppb free-running offset). ✅
- RUN starts T605: P-term clamp (±2000) hits immediately on first tick. ✅
- iAccumulator pull-in: 32767 → ~23750 by T1800 at ~11 counts/tick initially, slowing to ~0.03 counts/tick at T1800–T2000. OCXO was near on-frequency here (`timerCounterReal ≈ 0`). ✅
- **Then the loop did NOT hold at ~23750.** iAccumulator resumed drifting downward, accelerating to ~13 counts/tick by T4200, and eventually hit DAC=0 at T6267. ⚠️
- After T6267, `iAccumulator` stuck at 0.0 permanently. `timerCounterError` = +7 to +11 (OCXO now running slow), `ticCorrectedNetValueFiltered` = −30 to −50. **The I-step was always negative (pushing further into the floor), so it was discarded by the clamp — but the clamp only discarded `iRemainder`, not the step itself. The next tick computed a fresh negative step and hit the rail again. Infinite repeat.** ⚠️
- **Root cause (two bugs):**
  1. **`iAccumulatorLast` was computed as `iAccumulator - iStepFloor` after clamping**, not captured before the step. This made lockDetection drift measurement inaccurate.
  2. **No true anti-windup**: when `iAccumulator` is clamped at a rail and the I-step is driving it deeper into that rail, the step (and remainder) should be discarded entirely. The old code zeroed `iRemainder` but still applied `iStepFloor` next tick as soon as remainder was clear.
- **Fix applied:** `iAccumulatorLast` now captured before any step modification. Anti-windup now checks: if at `dacMinValue` and `iStepFloor < 0`, or at `dacMaxValue` and `iStepFloor > 0`, the step is discarded. Only steps that move **away from** the rail are applied.
- T1101 anomaly: `TIC Frequency Error = 0.00, TIC delta = 0.0000` — identical TIC value two consecutive ticks. Not a bug. ✅
- No missed PPS events across the full run. ✅

### Step 6 — Validate and tune
- ✅ First lock achieved at T1179 in run2.log. Loop is working correctly.
- run3.log + run3-2.log (T604–T5739+): No lock declared. iAccumulator drifted from 32757 → ~7700. Still converging at run end.
- run4.log (T158–T9113): **Anti-windup bug found and fixed.** iAccumulator hit DAC=0 at T6267 and stayed stuck. Two bugs:
  - `iAccumulatorLast` was computed after clamping instead of before the step.
  - I-steps that push deeper into a saturated rail were not being discarded — only `iRemainder` was cleared, so the next tick would compute a fresh step and hit the rail again.
- **Fix applied in `piLoop()`:** `iAccumulatorLast` captured before step. Anti-windup discards any I-step (and zeros remainder) when accumulator is already at a rail and the step would move further into it. Steps that move away from the rail are still applied normally.

### log 2026-03-14-run5.log
- **Run T13–T1353 (1340 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- Anti-windup fix confirmed working: `iAccumulator` converged cleanly from 32767 → ~24430 at T1353. No runaway. ✅
- `timerCounterReal` oscillating ±1 to ±2 at end of run — OCXO very close to on-frequency. ✅
- EFC setpoint consistent with run2 (~24250) and run4 (~23750 at T1800) — confirms same ambient temperature. ✅
- **P-term clamp (2000) found to be too large:**
  - P-term hits ±2000 clamp on **75% of all RUN-mode ticks**.
  - Even on normal (non-wrap) ticks: clamp fires on **58%** of them.
  - Mean `|ticFrequencyError|` on non-wrap ticks ≈ 192 counts × gain 12 = **2304 DAC counts** — exceeds the 2000 clamp on average.
  - DAC swings ±2000 counts (±0.15 V) almost every tick — masking the actual P signal and injecting large noise.
  - The clamp is 320× a typical I-step (~6 counts/tick at filtered=50) — far too unbalanced.
- **Fix applied:** `PTERM_MAX_COUNTS` reduced from 2000 to **200**.
  - At gain=12, the clamp now fires only when `|ticFrequencyError| > 17 counts`.
  - Sawtooth wraps (typically ±500–800 counts × 12 = ±6000–9600) are still hard-clamped to ±200.
  - Normal near-setpoint ticks pass through unclamped — genuine frequency-error damping.
  - DAC swing from P-term now ≤ ±200 counts (±0.015 V) per tick. Appropriate scale.
- No missed PPS events. ✅

### log 2026-03-14-run6.log
- **Run T9–T1535 (1526 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- `iAccumulator` converged from 32767 → ~24610 at T1535. Consistent with previous runs. ✅
- **Bug identified:** P-term was using `ticFrequencyError` = `ticDelta + timerCounterError × 200`.
  - `timerCounterError × 200` fires at ±200 ns on every GPS PPS jitter tick of ±1 count.
  - At gain=12 that produces ±2400 DAC counts, hitting the ±200 clamp on **92% of all RUN ticks** (88% of non-wrap ticks).
  - On `timerCounterReal=0` ticks, mean `|ticDelta|` = 100 counts (real signal). On `timerCounterReal=±1` ticks, combined `|ticFrequencyError|` = 212 counts — entirely due to the coarse term, not the OCXO.
  - GPS PPS jitter of ±1 count is **not a real frequency error** and should not drive the P-term.
- **Fix applied:** P-term now uses `ticDelta * gain` instead of `ticFrequencyError * gain`.
- **Fix applied:** P-term now uses `ticDelta * gain` instead of `ticFrequencyError * gain`.
  - `ticFrequencyError` (with coarse term) is still computed and logged for diagnostic purposes.
  - Sawtooth wraps still produce large `ticDelta` spikes and are still clamped by `PTERM_MAX_COUNTS`.
  - With `ticDelta` as the source, `PTERM_MAX_COUNTS` restored to **2000**: the sawtooth ramp at ~170 counts/s × gain 12 = ~2040 counts is a real signal that should pass; wrap spikes at ~500–800 counts × gain 12 = 6000–9600 are correctly clamped. The 200-count clamp was only needed because `coarseFreqError` was inflating every tick — with `ticDelta` that problem is gone.
- No missed PPS events. ✅

### log 2026-03-14-run7.log
- **Run T24–T1608 (1584 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- `iAccumulator` converged from 32767 → ~24223 by T1608. Consistent with previous runs (~24250). ✅
- `iAccumulator` drift rate by end of run: **+0.08/tick** — essentially zero. Loop fully converged. ✅
- `timerCounterReal` oscillating ±1–2 — OCXO very close to on-frequency. ✅
- **No lock declared** — `ppsLockCount` reached maximum of **7** consecutive OK ticks vs required 32. ⚠️
- **Root cause:** `LOCK_INTEGRATOR_DRIFT_MAX` guard was the blocker. Once converged, the I-step oscillates ±(filtered×0.125) each tick following the TIC sawtooth. Even though the *mean* drift is ~0.08/tick, the *per-tick* step is ±7.5 counts — permanently above the 2.0-count threshold. The guard was correctly rejecting premature lock during pull-in but incorrectly blocking lock after convergence.
- **Separately:** even with only the phase condition, max consecutive `|filtered|<50` was **30 ticks** — just 2 short of the 32 threshold. The loop was essentially locked but the count kept resetting when the TIC sawtooth swung the EMA above ±50.
- **Fix applied:** `LOCK_INTEGRATOR_DRIFT_MAX` guard removed from `lockDetection()`. Lock now requires only `|filtered| < LOCK_THRESHOLD` for `2 × ticFilterConst` consecutive seconds. `LOCK_INTEGRATOR_DRIFT_MAX` constant removed from `Constants.h`.

### log 2026-03-14-run8.log
- **Run T9–T1415 (1406 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- **LOCKED declared at T1371** (`ppsLockCount` = 32 = 2 × ticFilterConst). ✅ First lock with the iDrift guard removed.
- `iAccumulator` at lock: ~24182 counts = 1.845 V. Consistent with all previous runs (~24100–24250). ✅
- `ppsLockCount` holds at 32 continuously from T1371 to end of log. ✅
- `timerCounterReal` at end: −1 to 0. `ticCorrectedNetValueFiltered` = +20 at final tick. OCXO very close to on-frequency. ✅
- TIC sawtooth still active (full range visible) — P-term still swinging ±500–2000 counts. More run time needed for sawtooth to compress. ✅
- `iAccumulatorLast` field and `LOCK_INTEGRATOR_DRIFT_MAX` constant removed (dead code cleanup). ✅
- No missed PPS events. ✅

### log 2026-03-14-run9.log
- **Extended run T8–T6791 (6783 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- **Temperature sensors added to log:** `Temp OCXO` (sensor below OCXO) and `Temp Board` (sensor near TIC capacitor). ✅
- **Box removed** — hardware back in open air. OCXO steady at 30.62–30.75 °C throughout. Board at 22.25 °C. ✅
- **LOCKED declared at T1447** (`ppsLockCount` = 32). ✅
- **No unlock events across the full 6783-second run.** ✅
- `iAccumulator` converged and stable from ~T3600 onward at **~22,787 counts = 1.739 V**. ✅
  - Final 1000s: mean = 22,787, std dev = **39 counts = 3 mV** — essentially static.
  - Mean filtered = **+0.67 counts**, mean I-step = **+0.084/tick** — loop fully at its null.
- **TIC sawtooth did NOT compress.** Sawtooth period stuck at 6–7 ticks throughout the settled region. ⚠️
- **Root cause identified — hardware limit:** `timerCounterReal` distribution in the final 1000s is centred at −0.78 (mostly −1), meaning the OCXO is running **~156 ppb fast** even with the integrator fully settled at 1.739 V. The EFC range at this ambient temperature cannot bring the OCXO to true zero frequency error. The loop has found the best DAC value it can but cannot close the residual 156 ppb offset. This is an OCXO hardware characteristic, not a software bug.
- The 6–7 tick sawtooth period is exactly consistent with 156 ppb drift: TIC range = 100 ns, 100 ns / 156 ns/s ≈ 0.64 s ≈ 6–7 ticks. ✅
- No missed PPS events. ✅
- **Unlock bug identified:** single-tick immediate unlock fires on TIC sawtooth peaks brushing ±100 counts, resetting the 32-second re-lock counter unnecessarily. ⚠️ (fix reverted — kept as single-tick unlock for now)

### Step 6 — Validate and tune (running summary)
- run2.log: ✅ First lock at T1179.
- run4.log: Anti-windup bug fixed.
- run5.log: P-term clamp 2000→200 (then restored — see run6).
- run6.log: P-term source changed `ticFrequencyError`→`ticDelta`. `PTERM_MAX_COUNTS` restored to 2000.
- run7.log: iDrift guard removed from `lockDetection()`.
- run8.log: ✅ First lock with all fixes in place. T1371.
- run9.log: Temperature sensors added. Unlock hysteresis fix attempted but reverted — kept as single-tick unlock.
  - **Extended run (6783 s):** iAccumulator fully converged at 22,787 counts = 1.739 V. Mean filtered = +0.67, mean I-step = +0.084/tick. Loop is at its null.
  - TIC sawtooth stuck at 6–7 ticks — **hardware limit**: OCXO runs ~156 ppb fast even at EFC null. timerCounterReal distribution centred at −0.78 in settled region. The loop is correct; the OCXO's EFC range cannot reach true 0 ppb at this temperature. Coarse trim loop (Step 7) implemented to address this.
- If the loop oscillates: increase `damping` or `timeConst`.
- If the loop is too slow to pull in: decrease `timeConst` or increase `gain`.
- Run for 8000–10000 seconds total after coarse trim enabled (watch `lastCoarseTrim` in log; expect `timerCounterReal` mean to shift toward 0).
- Watch for sawtooth period increasing (means OCXO is closer to on-frequency and drift rate is falling).
- Watch `ticFrequencyError` on normal ticks approaching zero — that is the true indicator of frequency lock.
- When `ticFrequencyError` on non-wrap ticks is consistently ±10 counts or less, the OCXO is essentially on-frequency.
- **Note:** Consider increasing `WARMUP_TIME_DEFAULT` to 900–1200 s to allow better thermal stabilisation before RUN mode engages.

### ~~Step 7 — Coarse frequency trim~~ ✅ Done (implemented in `piLoop()`)
- `coarseErrorAccumulator`, `coarseTrimGain` (default 0.5), `coarseTrimPeriod` (default 64 s), `lastCoarseTrim` added to `ControlState`.
- Every tick: `coarseErrorAccumulator += timerCounterError`.
- Every `coarseTrimPeriod` seconds: `iAccumulator += coarseErrorAccumulator * coarseTrimGain`, accumulator reset to 0.
- Anti-windup: trim discarded if `iAccumulator` is already at a rail and trim would push further in.
- `lastCoarseTrim` recorded for logging (0 on non-trim ticks).
- Corrects residual frequency offsets (e.g. OCXO running 156 ppb fast) that the fine TIC phase loop nulls in phase but cannot correct in frequency.
- Identified as necessary from run9.log where `timerCounterReal` settled at −0.78 (i.e. ~156 ppb residual offset).

### ~~Step 8 — EEPROM persistence~~ ✅ Done
- `ExternalEEPROMController` implemented with 24LC128, 8 rotating banks × 2048 bytes, wear-levelled round-robin.
- Saves `dacValue` (uint16_t) + `iAccumulator` (double) = 10-byte payload per bank.
- Write-before-header ordering: payload written first, header (magic + seq) committed last — power-safe.
- `begin()` scans all banks on startup, selects highest valid sequence number.
- `loadState()` returns `DEFAULT_EEPROM_STATE` on cold boot (seeds both fields from `DAC_INITIAL_VALUE`).
- `setEEPROMState()` called unconditionally on boot — seeds DAC and integrator, zeroes `iRemainder`, drives DAC hardware immediately.
- `isValid()` public accessor used in `setup()` to log warm vs cold boot.
- Three-phase save cadence driven by `storeStateTime` (seconds in RUN mode only):
  - Phase 1 (< 1 hour): every 10 minutes
  - Phase 2 (< 12 hours): every hour
  - Phase 3 (≥ 12 hours): every 12 hours
- No saves during WARMUP or HOLD modes.
- Serial `'s'` command triggers a manual save; `'i'` invalidates all banks (forces cold boot next restart).
- `iRemainder` and `dacVoltage` are never stored — always re-derived on load.

### log 2026-03-20-run1-bis.log
- **Run T93–T3563 (3470 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- **Warm boot confirmed:** `iAccumulator` seeded at 22880 (from EEPROM). ✅
- Loop converged nicely from T604 → T640: `iAccumulator` pulled from 22880 → ~22730 in the first 36 ticks of RUN mode. ✅
- **LOCKED declared** at approximately T1447 (first occurrence of `ppsLockCount = 32`). ✅
- `iAccumulator` stable at ~23,150–23,200 through T2960. OCXO temp rising slowly from 28.37 → 29.87 °C. ✅
- **LOOP COLLAPSED at T3008.** `iAccumulator` dropped instantly from ~23,184 to **0**. DAC crashed to 0. OCXO ran ~700–800 ppb fast. Loop never recovered. ⚠️
- **Root cause — coarseErrorAccumulator poisoned by glitch `timerCounterError` spikes:**
  - T2964: `timerCounterError = -29,949,999` (hardware glitch — spurious overflow-counter increment)
  - T2967: `timerCounterError = -9,350,000` (second glitch)
  - Both were accumulated unconditionally into `coarseErrorAccumulator`, driving it to **−115,549,952**
  - T3008 (`time % 64 == 0`): coarse trim fired: `coarseTrim = -115,549,952 × 0.5 = -57,799,976`
  - Applied to `iAccumulator` (~23,184): result clamps to 0. DAC = 0. Loop dead.
- **Fix applied:** `coarseErrorAccumulator` now guarded by `COARSE_ERROR_SANITY_LIMIT = 50`.
  - Any tick where `|timerCounterError| > 50` is silently skipped — not accumulated.
  - Normal operation range is ±1–5; 50 counts (= 10 µs/s = 10 ppm) is unambiguously a glitch.
  - The anti-windup rail check was working correctly but had no defence against a single poison value accumulated 64 s in advance.
- **New constant added to `Constants.h`:** `COARSE_ERROR_SANITY_LIMIT = 50` (int32_t).
- No missed PPS events prior to the crash. ✅

### log 2026-03-20-run2.log
- **Run T604–T2244+ (mode RUN from T604).** Loop locked correctly, `iAccumulator` stable at ~22,800 counts. ✅
- **LOOP COLLAPSED at T2048.** Same symptom as run1-bis: `iAccumulator` → 0, DAC → 0. ⚠️
- **Root cause — two separate bugs in the previous fix:**
  1. **`abs()` is unsafe for `int32_t` on AVR.** Arduino's `abs()` macro operates on `int`, which is 16-bit on AVR. Calling `abs(state_.timerCounterError)` on a value like `−29,949,998` silently truncates to 16-bit before taking abs, yielding a small garbage value (e.g. ≈ 2) that passes the `<= 50` guard — so the sanity check was silently no-oping for the exact glitch values it was designed to catch.
  2. **`timerCounterError` itself was computed with `unsigned long` overflow.** `lastOverflow * MODULO` is evaluated in `unsigned long` arithmetic. If `lastOverflow` is e.g. 599 (a spurious large value from a missed PPS ISR accumulating multiple seconds of overflow counts), the product is 29,950,000 — within `unsigned long` range but wraps to a negative `int32_t` when cast. The `static_cast<int32_t>` was masking the problem by converting the large unsigned product to a valid-looking negative int32.
- **Fixes applied (two changes to `CalculationController.cpp`):**
  - **`timerCounterNormalization()`**: `lastOverflow` is now clamped to ≤ 200 before use (100 overflows/s is normal; 200 is the absolute maximum for a sane PPS interval). This prevents the `lastOverflow * MODULO` product from ever overflowing `int32_t`, keeping `timerCounterError` in a physically meaningful range.
  - **Coarse accumulator guard**: replaced `abs(state_.timerCounterError) <= COARSE_ERROR_SANITY_LIMIT` with the explicit safe form `state_.timerCounterError >= -COARSE_ERROR_SANITY_LIMIT && state_.timerCounterError <= COARSE_ERROR_SANITY_LIMIT` — no integer type ambiguity possible.
  - **`lockDetection()`**: `abs(ticCorrectedNetValueFiltered)` replaced with `fabs()` (the correct function for `double`).

- **Non-recovery bug also identified and fixed:** Even with the crash prevented, if a glitch somehow drives `iAccumulator` to a rail, the loop could not self-recover. The coarse trim injects ~238 counts per period but the I-term drains it in ~19 ticks at typical post-crash filtered error levels — the trim can never win. Fixed by adding **I-term suppression after a rail-recovery coarse trim**:
  - New field `iTermSuppressCount` (int32_t, default 0) added to `ControlState`.
  - When the coarse trim fires and moves the accumulator **away from** a rail (`trimAway` = true), `iTermSuppressCount` is set to `coarseTrimPeriod` and `iRemainder` is zeroed.
  - Each tick while `iTermSuppressCount > 0`, the I-step is skipped entirely and the count decrements.
  - This gives the coarse trim one full period to push the accumulator clear before the I-term resumes.

### log 2026-03-20-run3.log
- **Run T150–T6782 (6632 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- **Warm boot confirmed:** `iAccumulator` seeded at 22880.0 from EEPROM. ✅
- **LOCKED declared** and held continuously: `ppsLockCount = 32` throughout. ✅
- `iAccumulator` stable at ~22,960–22,990 counts = 1.753 V. Consistent with run1-bis. ✅
- **The loop did NOT crash.** All glitch events were correctly absorbed. ✅
- **Three glitch types observed around T2280–T2319:**

  **Type 1 — GPS ±10 ms timing jitter (T2280/T2281, T2290):**
  - `timerCounterError` = ±50000 (GPS PPS arrived ~10 ms early, then compensated).
  - These are genuine GPS timing anomalies, not software bugs.
  - `COARSE_ERROR_SANITY_LIMIT = 50` correctly blocked all three from `coarseErrorAccumulator`. ✅
  - `iAccumulator` unchanged on these ticks. ✅

  **Type 2 — Multi-second PPS gaps (T2292 through T2305, time jumps of 2–4 s):**
  - The GPS module suppressed its PPS output for 2–4 seconds (probable brief loss of GPS lock).
  - Each of these ticks had `lastOverflow` = 200–400, clamped to 200, producing `timerCounterError ≈ -5,000,000`.
  - Sanity guard correctly blocked all of them. `iAccumulator` unchanged. ✅
  - Recovery was immediate — the next normal tick resumed clean operation. ✅

  **Type 3 — Wrap-around detection miss (T2319, TCReal = 49999):**
  - `timerCounterValueReal = localTimerCounter - timerCounterValueOld` produced +49999 when the TCA0
    counter captured a value just below the previous snapshot (upward wrap).
  - The old wrap-around code only corrected **downward** wraps (`if < -MODULO/2: += MODULO`).
  - An upward wrap (diff > +MODULO/2) was not corrected, giving `TCReal = +49999` instead of `−1`.
  - Impact on this run was benign (sanity guard blocked coarse accumulator; pTerm clamped).
  - **Fix applied:** added `else if (timerCounterValueReal > (MODULO / 2)) { -= MODULO; }` to
    `timerCounterNormalization()`. Both wrap directions are now handled symmetrically.

- No lock drops across the full 6632-second run. ✅
- `coarseErrorAccumulator` remained in the expected range (≤54 counts at end). ✅
- No missed PPS events triggered (all gaps handled as multi-second PPS intervals). ✅

### log 2026-04-10-run1.log
- **Run T30–T2425 (2396 seconds total).** Mode transition WARMUP→RUN at T603. ✅
- **Warm boot confirmed:** `iAccumulator` seeded at 22,699.5 from EEPROM. DAC started at 20,699 (last hardware DAC value from previous session). ✅
- **LOCKED declared at T648** — only **45 seconds after entering RUN mode**. ✅ This is ~10× faster than cold-start runs (which typically lock around T1100–T1447). EEPROM warm-start seeding working as designed.
- **Lock held continuously from T648 to T2425 (1777+ seconds). Zero unlock events.** ✅
- `iAccumulator` at lock: 22,494 counts ≈ 1.718 V. Drifted slowly upward to ~23,580 (1.799 V) by T2425.
- **Drift is thermally driven**: OCXO temp rose from 32.63 °C at T603 to 33.38 °C at T2425. OCXO had not reached thermal equilibrium; `iAccumulator` drift rate slowed from ~0.9 counts/tick (early) to ~0.10 counts/tick (T2425). Full convergence expected ~1000–2000 seconds beyond log end once thermal equilibrium is reached.
- **Coarse trim working correctly:** First trim at T640 (`Coarse Trim = 18.0`), second at T704 (`Coarse Trim = 29.5`), subsequent trims every 64 s at ~19–30 counts each. No sanity guard trips. ✅
- **Residual frequency error improved vs run9:** `timerCounterReal` distribution post-lock centred near −0.5 (vs −0.78 in run9). Implies ~50–60 ppb residual vs ~156 ppb in run9. Coarse trim successfully correcting residual frequency offset. ✅
- TIC sawtooth still full-range (~0–810 counts) but period has lengthened to 8–9 ticks by end of log (vs 6–7 ticks in run9), consistent with the reduced residual frequency error.
- `ticFrequencyError` on non-wrap ticks: ~100–170 counts early, declining to ~50–130 counts by T2000+. Trend toward zero as OCXO approaches on-frequency.
- No GPS glitch events. No missed PPS events. ✅
- **Summary: EEPROM warm-start + coarse trim combination has reduced lock time from ~800 s to ~45 s and residual frequency error from ~156 ppb to ~50–60 ppb.** Both features validated as working correctly.

### TIC polynomial refit — C0G/NP0 capacitor for C9 (2026-04-10)
- **C9 changed from X7R to C0G/NP0 1 nF** prior to the 2026-04-10-run1 session. C0G has near-zero voltage coefficient, eliminating the charge-ramp curvature that the original X7R polynomial was correcting.
- `utils/fit_tic_poly.py` written and run against the run1 log. It implements the exact firmware Horner-form model:
  `s = (tic - TIC_MIN)/(TIC_MAX - TIC_MIN)*1000;  linearized = s*(x1 + s*(x2 + s*x3))`
  where `x1 = 1 - x2*1000 - x3*100000` (unity-gain constraint). Supports `--plot`.
- **Refitted cubic from C0G log:** x2 ≈ 1.0e-4, x3 ≈ 3.0e-7 (identical to current defaults), RMS = 0.003 counts — the log just reflects what the firmware is already computing, not an independent hardware measurement.
- **Step-variance metric unusable at cold-boot**: OCXO ~200 ppb off-frequency → TIC sawtooth wraps every 2–3 ticks → no extended ramp to measure non-linearity from.
- **Cross-check on X7R run3 log**: "no correction" (x2=x3=0) gave step-std = 9.2 vs current polynomial = 24.0 → the old X7R polynomial was *worsening* linearity, suggesting it was fitted under different conditions and should not be trusted for the C0G.
- **Action:** `x2Coefficient` and `x3Coefficient` set to **0.0** in `src/Constants.h` (C0G). Was 1.0e-4 / 3.0e-7 for X7R.
  - With zero coefficients: `linearize(x) = (x - TIC_MIN)/(TIC_MAX - TIC_MIN)*1000` — linear scaling only.
  - `ticValueCorrectionOffset = linearize(500) = 488.0`; loop still centres on zero automatically.
- **To refit properly for C0G:** wait until the loop is locked and the sawtooth period ≥ 10 ticks, then run:
  `python3 utils/fit_tic_poly.py <long-locked-log> [--plot]`

### log 2026-04-10-run2.log
- **Run T14–T1657 (1643 seconds total).** Mode transition WARMUP→RUN at T603. ✅
- **Warm boot confirmed:** `iAccumulator` seeded at 23467.0, DAC Value 25467 (= 1.943 V) from EEPROM. ✅
- **C0G zero-coefficient TIC linearisation confirmed working:**
  - `TIC Correction Offset = 488.00` (= `linearize(500) = (500−12)/1000×1000 = 488`). ✅
  - `TIC Correction = raw − 12` throughout. Linear scaling with no polynomial curvature. ✅
- **LOCKED declared at T821** (~218 s into RUN mode). ✅
- `iAccumulator` at lock: ~22391 counts ≈ 1.710 V. ✅
- **Coarse trim working correctly:** first trim at T640 (+12.5 counts), subsequent trims every 64 s, all negative, growing in magnitude: 12.5 → 26 → 29 → 32 → 33.5 → 37 → 39 → 45 → 51.5 → 57.5 → 63 → 74.5 → 87.5 → 96 → 111 → 120.5 counts. ✅ (all < COARSE_ERROR_SANITY_LIMIT × coarseTrimGain in per-tick terms)
- **Lock broken at T1251** — `iAccumulator` had drifted from ~22391 at T821 to ~20122 at T1251: ~2269 counts in 430 s ≈ **−5.3 counts/tick** average downward drift. ✅ (lock correctly broken; this is expected behaviour, not a bug)
- **After T1251, loop continues to drift downward** throughout remainder of log:
  - `iAccumulator` at T1657 (end of log): ~14907 counts ≈ 1.139 V. Still drifting at ~17 counts/tick.
  - `timerCounterReal` consistently −3 to −5 from ~T1200 onward — OCXO running ~600–1000 ppb fast at the current EFC voltage, confirming the loop is far from the true setpoint.
- **Root cause of persistent divergence — EFC setpoint has shifted dramatically lower since run1:**
  - At run1 end (T2425), settled at ~23580 counts (1.799 V).
  - This run's trajectory suggests true on-frequency setpoint is in the range **8000–12000 counts (0.6–0.9 V)**.
  - This is ~11,000–15,000 counts lower than run1. The OCXO's EFC sensitivity at this ambient temperature produces a fundamentally different setpoint.
  - **Most likely cause:** significant OCXO temperature increase between run1 and run2 — or the hardware environment has changed (e.g. enclosure, airflow, lab temperature). OCXO was 33.38–33.50 °C throughout (same as run1), but board/ambient may differ.
- **Coarse trim is accelerating, not converging:** Because each trim pushes the DAC further downward and the OCXO's frequency error worsens (more negative `timerCounterReal`) as the EFC drops below the true setpoint, the per-trim correction grows with each period. The loop has not had enough time to pull the `iAccumulator` all the way down to the true setpoint.
- **This is NOT a software bug.** The loop is behaving correctly:
  - I-term and coarse trim are both pushing `iAccumulator` downward in response to the OCXO running fast.
  - The issue is that the run was too short — at ~17 counts/tick drift rate near the end, the true setpoint is still ~8000–10000 counts away (~470–590 s of additional drift needed, plus convergence time).
  - Given the I-term and coarse trim both active, true convergence requires ~2000–3000 additional seconds beyond T1657.
- **No GPS glitch events, no missed PPS events.** ✅
- **Sawtooth period** throughout RUN mode: 5–7 ticks (consistent with 150–200 ppb free-running offset from true setpoint). Did not compress, confirming the OCXO was never close to on-frequency at any DAC value visited in this run. ✅
- **EEPROM save:** last saved `iAccumulator`/`dacValue` will be from the save cadence during RUN. On next warm boot, the seeded value will be much closer to the true setpoint and convergence will be faster. ✅
- **Action for next run:** Let run for at least **3000–4000 seconds in RUN mode** (total ~4600–5200 s) to allow the loop to fully converge to the true setpoint at this OCXO temperature. Watch for `timerCounterReal` to centre on 0 and `ticFrequencyError` on non-wrap ticks to drop below 50 counts.
- **Run extended to T2271.** `iAccumulator` continued falling: 14907 → 8690 (0.66 V) at end. Drift rate ~8 counts/tick at end. EEPROM last saved at T1803 with `iAccumulator = 12849` — that is the next warm-boot seed (much closer to the true setpoint than the original 23467).
- **Overshoot identified and root cause confirmed:** The EMA filter (ticFilterConst=16) builds up 16 seconds of phase history during the long pull-in. When the OCXO crosses the true on-frequency EFC setpoint, `ticCorrectedNetValue` changes sign immediately but `ticCorrectedNetValueFiltered` takes ≥16 s to follow. During that lag the I-term continued integrating in the original direction — drilling the DAC ~3000+ counts past the setpoint before the filter caught up.
- **Fix applied:** Conditional integration (overshoot guard) added to `piLoop()`. When `ticCorrectedNetValue` and `ticCorrectedNetValueFiltered` have **opposite signs**, the I-step is skipped and `iRemainder` is zeroed. The integrator resumes as soon as raw and filtered agree in sign again. This is equivalent to standard back-calculation anti-windup — the integrator freezes while the filter memory is stale. Has no effect on steady-state locked behaviour (raw and filtered virtually always share sign within ±50 counts of lock).
- **Run terminated early** (near 0.66 V, heading toward DAC=0). Restart with the warm-boot seed of ~12849.

### log 2026-04-11-run2.log
- **Run T14–T4240 (4226 seconds total).** Mode transition WARMUP→RUN at T604. ✅
- **Warm boot confirmed:** `iAccumulator` seeded at 22880.0, OCXO temp 30.75 °C. ✅
- `iAccumulator` converged to **~25,440 counts (1.940 V)** mean in the T2000–T4000 region (stddev 61 counts). ✅
- **`timerCounterReal` centred at 0** in settled region (T2000+): 1390 ticks at 0, 464 at −1, 227 at +1. OCXO is on-frequency. ✅
- **Coarse trim working correctly:** correcting the residual offset over time; `timerCounterReal` centred much better than run1 (~−0.78 in run9 vs ~0 here). ✅
- **No lock declared throughout the entire run** despite loop being on-frequency. ⚠️
- **Root cause of no lock:** TIC sawtooth remained full-range (0–818 counts) throughout.
  - TIC sawtooth wraps on GPS PPS jitter (±1–3 timer counter counts) even when OCXO is on-frequency.
  - `ticCorrectedNetValueFiltered` oscillates ±80–170 counts at the sawtooth frequency (~6–7 ticks).
  - `LOCK_THRESHOLD = 50` was too tight: the filter swing exceeds it on every sawtooth cycle.
  - `UNLOCK_THRESHOLD = 100` was also too tight: sawtooth peaks brush it even after lock.
  - Lock count hit 32 (at T635) and `ppsLocked` was briefly set, but `filtered` > 100 on sawtooth peaks caused immediate unlock. Lock was declared and broken every few ticks.
- **Fix applied:** `LOCK_THRESHOLD` raised from **50 → 80**, `UNLOCK_THRESHOLD` raised from **100 → 200**.
  - With a full-range sawtooth (0–818 counts, ticOffset 400), `filtered` oscillates roughly ±80–120 near the settled setpoint.
  - 80-count lock threshold: lock can be declared during the lower half of each sawtooth swing.
  - 200-count unlock threshold: sawtooth peaks (~170 counts) can no longer break lock.
  - These values were validated against the run2 data — the loop would have locked and held with these thresholds.

### ~~Step 5 — Lock detection~~ ✅ Done (validated in run8/run9/run2-apr11)
- Added to `ControlState`: `ppsLocked` (bool), `lockCount` (int32_t).
- Added to `Constants.h`: `LOCK_THRESHOLD` (80.0), `UNLOCK_THRESHOLD` (200.0).
- Private method `lockDetection(OpMode mode)` implemented in `CalculationController`.
- Resets `ppsLocked` / `lockCount` when mode is not `RUN`.
- Requires `lockCount ≥ ticFilterConst` consecutive seconds below `LOCK_THRESHOLD` to declare lock.
- Unlocks immediately when `abs(ticCorrectedNetValueFiltered) > UNLOCK_THRESHOLD`.
- `LOCK_LED` is driven from `ppsLocked` in the main loop after each PPS event.
- Threshold values tuned from run2-apr11 analysis: 80/200 (was 50/100).
- Lock count requirement reduced from `2×ticFilterConst` to `ticFilterConst` (run3-apr11):
  With a 42-tick sawtooth and filterConst=16, filtered stays below 80 for only ~25 ticks per
  cycle. Requiring 32 consecutive ticks made lock structurally impossible. 16 consecutive ticks
  (one full EMA time constant) is achievable in every sawtooth trough and is sufficient evidence
  of convergence.

### log 2026-04-11-run3.log
- **Run T13–T1060 (short run, 457 RUN-mode ticks from T604).** Warm boot seeded from run2 EEPROM. ✅
- `iAccumulator` stable at ~25,395–25,488 (mean ~25,440, consistent with run2). ✅
- `timerCounterReal` centred at 0 (289 at 0, 87 at −1, 41 at +1). OCXO on-frequency. ✅
- **No lock declared — max lockCount reached was 31 (one short of the old 32 requirement).** ⚠️
- **Root cause:** TIC sawtooth period ~42 ticks, filterConst=16. Filtered signal swings −105 to +187.
  Only ~59% of ticks have `|filtered| < 80`. The lock window (~25 consecutive ticks per cycle)
  is shorter than the old 32-tick requirement. Every streak hit exactly 31 and was broken by the
  next sawtooth peak at T916 (filtered=87.1).
- **Fix applied:** lock count requirement changed from `2×ticFilterConst` (32) → `ticFilterConst` (16).
  16 consecutive ticks within ±80 counts is achievable in every sawtooth trough once converged.
- **Run T59–T997+ (WARMUP T59–T603, RUN from T603).** Warm boot: `iAccumulator` seeded at 22880, OCXO temp 21.87 °C (cold). ✅
- **Loop diverged in RUN mode:** `iAccumulator` drifted consistently downward from 22880 at T604 to ~20461 at T997 at ~6–10 counts/tick net. OCXO temp rising slowly toward 30 °C. ⚠️
- **Root cause — I-term structural sawtooth bias:**
  - The TIC sawtooth on this hardware spans TIC_MIN (~12) to ~780–815 hardware counts — NOT to TIC_MAX (1012).
  - Mean TIC during a sawtooth cycle ≈ (12 + 800) / 2 = 406. With `ticOffset = 500`, `ticValueCorrectionOffset = 488`.
  - Mean `ticCorrectedNetValue` during sawtooth = (406−12) − 488 = **−94** — a permanent negative bias regardless of whether the OCXO is fast or slow.
  - This bias produces I-steps of ≈ −94 × 12 / 3 / 32 ≈ **−11.75 counts/tick** downward — always, independent of true frequency error.
  - When the OCXO is SLOW (needs DAC to go UP), the I-term still drives DOWN due to this bias, causing divergence.
  - The coarse trim (positive, +14–43 counts per 64 s) is far too weak (~0.5 counts/tick) to overcome the −11.75 counts/tick I-term bias.
  - Previous runs converging correctly were starting from 32767 (OCXO fast), so "drive DOWN" was the correct direction. The bug was always present but the direction happened to be right. Starting from 22880 with a cold (slow) OCXO exposes the failure mode.
- **`timerCounterReal` = −1 to −6 (OCXO slow at 22880/cold boot), `timerCounterError` = +1 to +6.** Coarse trim is positive ✓ (trying to push DAC up), but overwhelmed by I-term.
- **P-term analysis:** `ticDelta` during sawtooth ramp ≈ +100–200, `pTerm = +1200–2000` (clamped). P-term is positive (pushing DAC up) ✓, contributing mean +1714 DAC counts per cycle. But iAccumulator still drifts DOWN due to I-term. Mean DAC seen by OCXO ≈ iAccumulator + 1714 ≈ 22000–24000 — too low for the cold OCXO. Loop diverges.
- **Fix applied:** `ticOffset` changed from **500.0 → 400.0** in `Constants.h`.
  - With `ticOffset = 400`, `ticValueCorrectionOffset = linearize(400) = 400 − 12 = 388`.
  - Mean `ticCorrectedNetValue` during sawtooth = (406−12) − 388 = **+6 ≈ 0** — bias eliminated.
  - I-term is now unbiased during sawtooth pull-in; coarse trim and true phase error drive the loop correctly.
  - At lock, equilibrium TIC = 400 (instead of 500). Both are valid phase offsets; the loop self-centres on ticOffset regardless.
  - The earlier `ticOffset = 500.0` assumption was documented as "midpoint of TIC range [TIC_MIN, TIC_MAX]" but TIC_MAX=1012 is the ADC clip limit, not the hardware sawtooth top. Actual hardware sawtooth top ≈ 780–815.

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
- **`EEPROMState` layout changes** require bumping the `kMagic` version byte in
  `ExternalEEPROMController.h` to invalidate stale banks on next boot.

---

## Keep this file up to date

After every session, update:
- **`ControlState` fields** if new fields were added or changed.
- **`EEPROMState` fields** if stored fields were added or changed (and bump `kMagic`).
- **Pipeline section** if new methods were added to `CalculationController`.
- **Validated section** if a new log confirmed correct behaviour.
- **Next steps** if a step was completed or a new one was identified.
