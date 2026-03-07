# EEPROM Layout — GPSDO Firmware

This document describes the on-device EEPROM layout used by the firmware in `src/` (authoritative: `src/EEPROMController.*`). It lists each stored value with byte offset, size, type, purpose, units (where applicable), and any defaults or important behaviour.

Summary
-------
- Storage is a packed struct named `PersistedState` defined in `src/EEPROMController.h`.
- Storage is little-endian and packed (no padding). The code reserves 100 bytes starting at EEPROM base address 0.
- A 4-byte magic value is used to mark a valid image. The magic constant is 0x47505344 ('GPSD' in ASCII). The magic is written last when saving to avoid partially-valid images.
- Total used bytes by the current layout: 77 bytes (<= the 100 byte reservation).

General notes
-------------
- Endianness: values are stored little-endian. The code reads/writes bytes explicitly; multi-byte values are reconstructed as little-endian integers/floats.
- Atomicity: `saveState()` writes the payload with the magic cleared, then writes the magic (LSB first) last. This prevents a partially-written payload from being considered valid.
- Validation: `begin()` reads the 4-byte magic at address 0 and sets an internal `isValid_` flag. `loadState()` returns false if the stored magic does not match `kMagic`.
- Layout changes: `kMagic` should be changed if the `PersistedState` layout changes in an incompatible way.

Byte map (offset, size, type)
-----------------------------
All offsets are byte offsets from the EEPROM base address (`kEepromBaseAddr == 0`). Sizes are in bytes.

- 0x00 (0) — 4 bytes — uint32_t `magic`
  - Stored value: 0x47505344 ('GPSD'). Written LSB-first: bytes 0 = 0x44, 1 = 0x53, 2 = 0x50, 3 = 0x47.
  - Purpose: validity marker. `loadState()` only attempts to read/restore state when this matches the compiled `kMagic`.

- 0x04 (4) — 4 bytes — float `iTerm`
  - Purpose: PI controller integrator term (short-term fractional part). Internal units: same arithmetic as in `CalculationController::calculate` (unitless scaled loop state).
  - Default (at runtime when not restored): 0.0

- 0x08 (8) — 4 bytes — int32_t `iTermLong`
  - Purpose: integer part of the integrator accumulator used when updating `dacValue`.
  - Default: 0

- 0x0C (12) — 4 bytes — float `iTermRemain`
  - Purpose: fractional remainder of integrator left after extracting `iTermLong`.
  - Default: 0.0

- 0x10 (16) — 4 bytes — float `pTerm`
  - Purpose: the proportional term snapshot (for diagnostics/continuity).
  - Default: 0.0

- 0x14 (20) — 4 bytes — int32_t `dacValue`
  - Purpose: internal accumulator for DAC control (scaled units). This is the accumulator that `iTermLong` is added to.
  - Important: `dacValue` is an internal (possibly large) signed accumulator and is stored verbatim.
  - Default: 0

- 0x18 (24) — 2 bytes — uint16_t `dacValueOut`
  - Purpose: scaled 16-bit DAC value saved for restoring output after restart.
  - Stored range: 0..65535. When loading, `dacValueOut` is promoted to controller `int32_t`.
  - Note: when saving, the code truncates `controller.state().dacValueOut` to 16 bits; values >65535 will be truncated.
  - Default: 0

- 0x1A (26) — 2 bytes — uint16_t `holdValue`
  - Purpose: Held DAC output value (used when device is in HOLD mode). Stored raw for restore.
  - Default: 0

- 0x1C (28) — 2 bytes — uint16_t `timeConst`
  - Purpose: controller time-constant (affects loop behaviour and filtering). See `CalculationController::state().timeConst`.
  - Default: 32

- 0x1E (30) — 1 byte — uint8_t `filterDiv`
  - Purpose: filter divider used to compute `filterConst = timeConst / filterDiv`.
  - Default: 2

- 0x1F (31) — 2 bytes — uint16_t `filterConst`
  - Purpose: stored approximation of the effective filter constant (kept for continuity). Recomputed at runtime as `timeConst / filterDiv`.
  - Default: 16

- 0x21 (33) — 2 bytes — uint16_t `ticOffset`
  - Purpose: TIC (ADC) offset calibration. Typical default in `ControlState` is 500.
  - Units: ADC counts (10-bit ADC used in main firmware)
  - Default: 500

- 0x23 (35) — 4 bytes — float `tempCoefficient`
  - Purpose: temperature compensation coefficient (degrees -> DAC correction scale).
  - Default: 0.0 (no compensation)

- 0x27 (39) — 4 bytes — float `tempReference`
  - Purpose: reference temperature (degrees C) used as baseline for temperature compensation.
  - Default: 30.0

- 0x2B (43) — 4 bytes — float `gain`
  - Purpose: PI loop gain (controller tuning). See `CalculationController::state().gain`.
  - Default: 12.0

- 0x2F (47) — 4 bytes — float `damping`
  - Purpose: damping factor used to compute integral term scaling.
  - Default: 3.0

- 0x33 (51) — 4 bytes — int32_t `ticValueFiltered`
  - Purpose: internal low-pass filter accumulator for TIC values. Saved so the filter state can be resumed across reboots.
  - Units: scaled ADC ticks (internal scaled value)
  - Default: 0

- 0x37 (55) — 4 bytes — uint32_t `restarts`
  - Purpose: long-term counter for power-ups / restarts. Incremented by `CalculationController::updateLongTermState` when the device first completes the 300s bucket.
  - Default: 0

- 0x3B (59) — 4 bytes — uint32_t `totalTime3h`
  - Purpose: number of completed 3-hour windows tracked since first measurement (increments every 3 hours).
  - Default: 0

- 0x3F (63) — 4 bytes — uint32_t `ticAverage3h`
  - Purpose: averaged TIC value over the last 3-hour window (derived from 300s buckets).
  - Units: scaled ADC ticks (see `ticArray` aggregation in `CalculationController`).
  - Default: 0

- 0x43 (67) — 4 bytes — uint32_t `tempAverage3h`
  - Purpose: averaged temperature (x10) over the last 3-hour window. See `CalculationController` where temperature readings are multiplied by 10 for storage in long-term sums.
  - Units: temperature * 10 (e.g. stored value 300 corresponds to 30.0 °C)
  - Default: 0

- 0x47 (71) — 4 bytes — uint32_t `dacAverage3h`
  - Purpose: averaged DAC output over the last 3-hour window.
  - Default: 0

- 0x4B (75) — 2 bytes — uint16_t `k`
  - Purpose: small index/counter used in long-term aggregation (controller `longTermState.k`).
  - Default: 0

Total used bytes: 77 (0x4D)
Reserved EEPROM area: `kEepromMaxSize` = 100 bytes (addresses 0..99)

Mapping to runtime structures
-----------------------------
`loadState()` maps the persisted struct into the runtime `ControlState` and `LongTermControlState` (see `src/EEPROMController.cpp`). Key mappings:
- `p.iTerm` -> `controller.state().iTerm`
- `p.iTermLong` -> `controller.state().iTermLong`
- `p.iTermRemain` -> `controller.state().iTermRemain`
- `p.pTerm` -> `controller.state().pTerm`
- `p.dacValue` -> `controller.state().dacValue`
- `p.dacValueOut` -> `controller.state().dacValueOut` (promoted to 32-bit)
- `p.holdValue` -> `controller.state().holdValue`
- `p.timeConst` -> `controller.state().timeConst` (and `timeConstOld`)
- `p.filterDiv` -> `controller.state().filterDiv`
- `p.filterConst` -> `controller.state().filterConst`
- `p.ticOffset` -> `controller.state().ticOffset`
- `p.tempCoefficient` -> `controller.state().tempCoefficientC`
- `p.tempReference` -> `controller.state().tempReferenceC`
- `p.gain` -> `controller.state().gain`
- `p.damping` -> `controller.state().damping`
- `p.ticValueFiltered` -> `controller.state().ticValueFiltered`
- long-term values -> `controller.longTermState()` fields (`restarts`, `totalTime3h`, `ticAverage3h`, `tempAverage3h`, `dacAverage3h`, `k`)

Important behavioural notes
--------------------------
- The code protects against loading incompatible layouts by checking the magic. If you change the `PersistedState` layout in future, change `kMagic` so old data isn't misinterpreted.
- `dacValueOut` stored as 16-bit in EEPROM but `CalculationController` uses a 32-bit accumulator at runtime. Saving will truncate values >65535.
- The long-term aggregations run on a 300-second bucket and produce 3-hour averaged values written into the persisted fields. The firmware calls `saveState()` approximately every 3 hours (when `totalTime3h` increments) in `CalculationController::updateLongTermState`.
- `ticValueFiltered` stores internal filter state. Restoring it improves continuity of the filter across restarts but may cause transient behaviour if hardware conditions change while the device was off.
- `holdValue` is the value applied to the DAC when the device is in HOLD mode; restoring it helps preserve hold behaviour across restarts.

Recommendations for future changes
----------------------------------
- When changing the persisted struct layout, increment `kMagic` to invalidate previous images rather than adding complex compatibility code.
- If you need to expand the persisted footprint, increase `kEepromMaxSize` and update the static_assert accordingly. Prefer appending new fields to the end to remain somewhat forward-compatible.
- Consider storing `dacValueOut` as a full 32-bit value if the runtime accumulator can exceed 16 bits for long-term accuracy.
- Document any newly-added fields here (and include the byte offsets). Keep this doc in `docs/` and update when the saved layout changes.

References
----------
- `src/EEPROMController.h` and `src/EEPROMController.cpp` — authoritative definitions of the persisted layout and read/write behaviour.
- `src/CalculationController.h` / `src/CalculationController.cpp` — runtime data structures and defaults.
- `archive/OriginalCode.cpp` — legacy firmware (behaviour reference).

