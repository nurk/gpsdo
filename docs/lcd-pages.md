# LCD Pages — GPSDO Display Layout

The 20×4 HD44780 LCD cycles through **4 pages** (0–3).
The active page is controlled from the main loop (button press or automatic rotation).
The display refreshes every 215 ms, or immediately when the page changes.

All pages are 20 columns × 4 rows. Characters shown below use `│` borders for
clarity — those borders are **not** part of the actual output.

---

## Page 0 — GPS / Time / Temperature

Shows position, date/time, board temperatures and satellite count.

```
┌────────────────────┐
│Lat: 051° 30'45.6" N│  Row 0 — Latitude  (DDD° MM'SS.S" N/S)
│Lon: 000° 07'32.1" W│  Row 1 — Longitude (DDD° MM'SS.S" E/W)
│16/03/2026 14:22:05 │  Row 2 — Date DD/MM/YYYY and UTC time HH:MM:SS
│T1:22.5 T2:30.6 S:8 │  Row 3 — Board temp (T1), OCXO temp (T2), satellites (S)
└────────────────────┘
```

### Fields

| Row | Field | Source | Notes |
|-----|-------|--------|-------|
| 0 | Latitude | `GpsData.latitude` | Degrees, minutes, decimal seconds + N/S hemisphere. Shows `---° --'--.-"` if position invalid. |
| 1 | Longitude | `GpsData.longitude` | Same format + E/W hemisphere. Shows `---° --'--.-"` if position invalid. |
| 2 | Date | `GpsData.day/month/year` | `DD/MM/YYYY`. Shows `--/--/----` if date invalid. |
| 2 | Time | `GpsData.hour/minute/second` | `HH:MM:SS` (UTC). Shows `--:--:--` if time invalid. |
| 3 | T1 | `readTempFn()` | Board temperature near TIC capacitor (°C), 1 decimal place. |
| 3 | T2 | `readOCXOTempFn()` | OCXO temperature (°C), 1 decimal place. |
| 3 | S | `GpsData.satellites` | Satellite count. Shows `S:-` if invalid. |

### Example — no GPS fix yet

```
┌────────────────────┐
│Lat: ---° --'--.-"  │
│Lon: ---° --'--.-"  │
│--/--/---- --:--:-- │
│T1:22.5 T2:30.6 S:- │
└────────────────────┘
```

---

## Page 1 — DAC / Mode / Lock

Shows the current EFC DAC output, operating mode and PPS lock status.

```
┌────────────────────┐
│Dac Volts:   1.7468V│  Row 0 — label (11) + value right-aligned in 8 + "V" = 20
│Dac Value:     22880│  Row 1 — label (11) + value right-aligned in 9 = 20
│Mode:            Run│  Row 2 — label (6)  + value right-aligned in 14 = 20
│PPS:          Locked│  Row 3 — "PPS:" (4) + value right-aligned in 16 = 20
└────────────────────┘
```

### Fields

| Row | Field | Source | Notes |
|-----|-------|--------|-------|
| 0 | Dac Volts | `ControlState.dacVoltage` | 4 decimal places, V. Range 0.0000–5.0000 V. |
| 1 | Dac Value | `ControlState.dacValue` | Integer, 0–65535, right-aligned. |
| 2 | Mode | `opMode_` | `Heating` = WARMUP, `Run` = RUN, `Hold` = HOLD, right-aligned. |
| 3 | Lock | `ControlState.ppsLocked` | `Locked` or `Unlocked`, right-aligned after `PPS:` label. |

### Example — warming up, not yet locked

```
┌────────────────────┐
│Dac Volts:  2.5000V │
│Dac Value:     32767│
│Mode:        Heating│
│PPS:        Unlocked│
└────────────────────┘
```

---

## Page 2 — TIC / Frequency Error

Shows raw TIC measurement, filtered phase error, frequency error and coarse counter error.

```
┌────────────────────┐
│TIC Raw:         512│  Row 0 — label (8)  + value right-aligned in 12 = 20
│TIC filt:    -12.345│  Row 1 — label (9)  + value right-aligned in 11 = 20
│TIC err:     -12.345│  Row 2 — label (8)  + value right-aligned in 12 = 20
│Ctr err:          -1│  Row 3 — label (8)  + value right-aligned in 12 = 20
└────────────────────┘
```

### Fields

| Row | Field | Source | Notes |
|-----|-------|--------|-------|
| 0 | TIC Raw | `ControlState.ticValue` | Raw 10-bit ADC reading. Valid range 12–1012. |
| 1 | TIC filt | `ControlState.ticCorrectedNetValueFiltered` | EMA-filtered, offset-subtracted, linearised phase error (counts ≈ ns). Positive = phase leading. |
| 2 | TIC err | `ControlState.ticFrequencyError` | `ticDelta + timerCounterError × 200`. Combined frequency error in nanoseconds per second. |
| 3 | Ctr err | `ControlState.timerCounterError` | `COUNTS_PER_PPS − timerCounterValueReal − overflows × MODULO`. Should be near 0 when on-frequency. |

### Example — loop locked and on-frequency

```
┌────────────────────┐
│TIC Raw:         487│
│TIC filt:      2.341│
│TIC err:     -14.876│
│Ctr err:           0│
└────────────────────┘
```

---

## Page 3 — PI Loop Internals

Shows the integrator state, fractional remainder, proportional term and coarse trim accumulator.

```
┌────────────────────┐
│I Acc:   22880.000  │  Row 0 — label (6)  + value right-aligned in 14 = 20
│I Rem:        0.125 │  Row 1 — label (6)  + value right-aligned in 14 = 20
│P Term:    -456.789 │  Row 2 — label (7)  + value right-aligned in 13 = 20
│Crs Acc:    -12.500 │  Row 3 — label (8)  + value right-aligned in 12 = 20
└────────────────────┘
```

### Fields

| Row | Field | Source | Notes |
|-----|-------|--------|-------|
| 0 | I Acc | `ControlState.iAccumulator` | Integrator state in DAC counts. Converges to the true EFC setpoint. Range [dacMinValue..dacMaxValue]. |
| 1 | I Rem | `ControlState.iRemainder` | Fractional carry-forward from the I-step. Prevents truncation drift. Reset to 0.0 on boot. |
| 2 | P Term | `ControlState.pTerm` | P-term contribution this tick. Derived from `ticDelta × gain`, clamped to ±`PTERM_MAX_COUNTS` (±2000). |
| 3 | Crs Acc | `ControlState.coarseErrorAccumulator` | Running sum of `timerCounterError`. Applied to `iAccumulator` as a trim every `coarseTrimPeriod` (64 s). |

### Example — loop settled and locked

```
┌────────────────────┐
│I Acc:    22787.000 │
│I Rem:        0.084 │
│P Term:     456.000 │
│Crs Acc:     -3.000 │
└────────────────────┘
```

---

## Bug fix applied (2026-03-16)

`drawPageThree()` row 3 was missing the `lcd_.print(line)` call after `snprintf`.
The coarse accumulator value was computed but never written to the display.
Fixed in `LcdController.cpp`.

