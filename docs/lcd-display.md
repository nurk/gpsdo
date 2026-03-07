# LCD Display — Page and Field Definitions

This file documents the 20×4 LCD pages and how fields are formatted and when placeholders are shown. It's the authoritative reference for the `LcdController` behaviour.

## Hardware
- Display: 20 columns × 4 rows (HD44780 compatible, I2C expander via `hd44780_I2Cexp`).
- Navigation: rotary encoder moves between pages (0..3).

## Overview
There are four pages implemented by `LcdController`:
- Page 0 — Primary telemetry (GPS, date/time, temps, sats)
- Page 1 — Accuracy / short-term controller status
- Page 2 — DAC / hold/voltage view
- Page 3 — Long-term statistics (sigma, 3-hour average, restarts)

All rows fit within 20 characters.

## Page 0 — Primary telemetry (20×4)
The main page (page 0) presents GPS-derived values, date/time and temperature/satellite status.

Row 0: Latitude (DMS)
- Label: `Lat:`
- Format: `DDD° MM'SS.S" H` where H is hemisphere (N or S).
- Example: `Lat: 051° 30'26.5" N`
- Placeholder when position unavailable (`GpsData::isPositionValid == false`):
  - `Lat: ---° --'--.-"  ` (keeps width stable)

Row 1: Longitude (DMS)
- Label: `Lon:`
- Format: `DDD° MM'SS.S" H` where H is hemisphere (E or W).
- Example: `Lon: 000° 07'39.9" W`
- Placeholder when position unavailable: `Lon: ---° --'--.-"  `

Row 2: Date and Time
- Format: `DD/MM/YYYY HH:MM:SS` (24-hour clock)
- Example: `25/01/2026 14:05:09`
- Date placeholder when `GpsData::isDateValid == false`: `--/--/----`
- Time placeholder when `GpsData::isTimeValid == false`: `--:--:--`
- Combined placeholder: `--/--/---- --:--:--`

Row 3: Temperatures and satellites
- Format: `T1:XX.X T2:YY.Y S:Z`
  - `T1` = local temperature sensor (one decimal)
  - `T2` = OCXO temperature (one decimal)
  - `S` = number of satellites
- Example: `T1:23.4 T2:45.6 S:7`
- If satellites are invalid (`GpsData::isSatellitesValid == false`), the display shows `S:-` to indicate unknown/invalid.

## Page 1 — Accuracy / short-term controller status (20×4)
This page surfaces timing and loop information useful for diagnosing accuracy and short-term stability.

Row 0: PPS lock state and missed PPS counter
- Format: `PPS:LOCKED Miss:NNN` or `PPS:UNLOCKED Miss:NNN`
- Uses `ControlState::ppsLocked` and `ControlState::missedPps`.
- Note: the `Miss` counter is space-padded to width 3. Other numeric fields on this page use the printf formats shown in the "Exact printf formats" section below.

Row 1: Timing error and filter/time constants
- Format: `Err:+DDDDDD F:FFF T:TTT`
  - `Err` = `ControlState::diffNs` printed as signed nanoseconds (explicit sign shown).
  - `F` = `ControlState::filterConst` (width 3)
  - `T` = `ControlState::timeConst` (width 3)
- Example: `Err:+000123 F:016 T:032`

Row 2: Jitter and proportional term
- Format: `Jit:+DDDDD p:PP.P`
  - `Jit` = short-term jitter computed as absolute difference between `ticValueFiltered` and `ticValueFilteredOld` (displayed with a sign via the signed format; value is absolute so appears with a `+` sign).
  - `p` = `ControlState::pTerm` printed to one decimal with fixed field width.
- Example: `Jit:+0012 p:12.3`

Row 3: 3-hour TIC average and filtered temperature
- Format: `3hT:AAAAA tmp:TT.T`
  - `3hT` = `LongTermControlState::ticAverage3h` (width 5)
  - `tmp` = `ControlState::tempFilteredC` (width 4, one decimal)
- If `LongTermControlState::totalTime3h == 0` (no 3-hour aggregate yet), the page shows placeholder `3hT:----- tmp:--.-`.

## Page 2 — DAC / hold / voltage (20×4)
This page shows the internal DAC output (scaled units), the configured hold/target value, and the computed DAC output voltage.

Row 0: DAC current output (scaled internal units)
- Label/format (exact from code): `Dac: %05ld` — i.e. `Dac: ` followed by a zero-padded 5-digit signed/long field.
- Example: `Dac: 01234`

Row 1: Hold value / HVAL
- Format: `Hold: %05u` (width 5, zero-padded for the hold value as an unsigned int).
- Example: `Hold: 00000`
- Label/format (exact from code): `Dav Volt: %6.4fV` — the code prints `Dav Volt:` (note spelling) and the voltage with 4 fractional digits.
- Computation: `dacVoltage = (dacOut / DAC_MAX_VALUE) * 5.0` and printed as `Dav Volt: x.xxxxV`.
- Example: `Dav Volt: 1.2345V`
- Computation: `dacVoltage = (dacOut / DAC_MAX_VALUE) * 5.0` and printed as `Dac Volts: x.xxxxV`.
- Example: `Dac Volts: 1.2345V`

Row 3: Hold indicator
- When `OperationMode` is `hold` the firmware writes `HOLD mode` on row 3. When not in hold, this row is empty.

(Implementation note: the string `Dav Volt:` appears in the code as-is; if you prefer `Dac Volt:` or `DAC Volt:` for clarity, consider changing the source and keeping docs in sync.)

## Page 3 — Long-term statistics (20×4)
This page contains computed long-term statistics such as sigma and the 3-hour average plus restart count.

Row 0: Long-term sigma and sample count
- Format when samples available: `sigma:SSS.S n:NNN` where sigma is printed with one decimal using a width that includes leading zeros/padding as implemented.
- Example: `sigma:012.3 n:024`
- When insufficient samples (`n <= 1`) the placeholder shown is: `sigma:----- n:---`.

Row 1: 3-hour TIC average and restart count
- Format: `3h:AAAAA R:RRR` (both fixed width)
- Example: `3h:00123 R:005`

Rows 2..3: currently unused on this page (the code clears the LCD and prints the two lines above).

## Placeholders and invalid data handling
- GPS position/date/time/satellites use the `GpsData::is*Valid` flags to decide whether to show live values or placeholders.
- Long-term statistics (3-hour aggregates and sigma) require accumulated samples; the code displays human-friendly placeholders until enough data are available.

## Exact printf formats (implementation reference)
The following lists the actual printf-style formats used in `src/LcdController.cpp` so the displayed padding and alignment are unambiguous.
- Page 0:
  - Latitude / Longitude (DMS): `formatDMS()` — `snprintf(out, 21, "%03d\xDF %02d'%04.1f\" %c", ...)` producing `DDD° MM'SS.S" H` with degree glyph 0xDF.
  - Latitude/Longitude placeholders: `"---\xDF --'--.-\"  "` (keeps field width stable)
  - Date: `"%02u/%02u/%04u"`
  - Time: `"%02u:%02u:%02u"`
  - Temperatures & sats: `"T1:%04.1f T2:%04.1f S:%u"` or `"T1:%04.1f T2:%04.1f S:-"`
- Page 1:
  - Row 0 (Miss): `"PPS:%s Miss:%3u"` (Miss is space-padded to width 3)
  - Row 1 (Err/F/T): `"Err:%+06ld F:%03u T:%03u"` (explicit sign for Err)
  - Row 2 (Jit/pTerm): `"Jit:%+05ld p:%04.1f"` (Jit shows sign; pTerm printed with one decimal and fixed width)
  - Row 3 (3hT/tmp): placeholder `"3hT:----- tmp:--.-"` or `"3hT:%05lu tmp:%04.1f"`
- Page 2:
  - Row 0 (DAC): `"Dac: %05ld"`
  - Row 1 (Hold): `"Hold: %05u"`
  - Row 2 (DAC voltage): `"Dac Volts: %6.4fV"`
  - Row 3 (Hold indicator): `"HOLD mode"` when `opMode_ == hold`
- Page 3:
  - Row 0 (sigma/n): `"sigma:%06.1f n:%03ld"` or placeholder `"sigma:----- n:---"`
  - Row 1 (3h/R): `"3h:%05lu R:%03lu"`

## Examples
Page 0 — valid GPS:
```
Lat: 051° 30'26.5" N
Lon: 000° 07'39.9" W
25/01/2026 14:05:09
T1:23.4 T2:45.6 S:7
```

Page 0 — no fix:
```
Lat: ---° --'--.-"  
Lon: ---° --'--.-"  
25/01/2026 14:05:09
T1:23.4 T2:45.6 S:7
```

Page 1 — accuracy view (with 3h data):
```
PPS:LOCKED Miss:  0
Err:+000123 F:016 T:032
Jit:+0012 p:12.3
3hT:00123 tmp:30.5
```

- `LcdController::drawPageZero()`, `drawPageOne()`, `drawPageTwo()` and `drawPageThree()` implement the pages described above.
```
Dac: 01234
Hold: 00000
Dac Volts: 1.2345V

```

Page 2 — hold active:
```
Dac: 01234
Hold: 00000
Dac Volts: 1.2345V
HOLD mode
```

Page 3 — sigma/3h:
```
sigma:012.3 n:024
3h:00123 R:005
```

## Implementation pointers
- `LcdController::drawPageZero()`, `LcdController::drawPageOne()`, `LcdController::drawPageTwo()` and `LcdController::drawPageThree()` implement the pages described above.
- `LcdController::formatDMS(...)` produces the DMS strings used on page 0 and appends hemisphere letters to keep the degree field width stable.
- Degree glyph: the firmware emits byte `0xDF` for a degree-like glyph. If your display shows a wrong glyph, define a custom CGRAM char or replace it in the formatter.
