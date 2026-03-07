# Calculation constants

This document lists the named constants introduced to replace "magic numbers" in `CalculationController.cpp` and why they were chosen.

Files changed
- `src/CalculationConstants.h` — calculation-only constants (private to the controller implementation).
- `src/Constants.h` — retains shared constants and public types (e.g. `DAC_MAX_VALUE`) and now includes `MAX_GPS_BYTES_PER_LOOP`.
- `src/CalculationController.cpp` — uses the named constants instead of literals.

New constants and rationale

- ADC_MAX_READING = 1023
  - 10-bit ADC maximum reading; used to detect invalid/initial ADC state (replaces literal 1023).

- TIMER_US_INCREMENT = 50000
- TIMER_US_SCALE_FACTOR = 200
- TIMER_US_BIAS = 50000500
- TIMER_US_DIVISOR = 1000
- TIC_NS_PER_US = 1000
  - These group the magic values used when updating `state_.timerUs` and converting between timer ticks/us/ns. Naming them clarifies the math and makes future changes easier.

- WARMUP_PRELOAD_MARGIN = 2
- WARMUP_RESET_THRESHOLD = 3
- WARMUP_RESET_MARGIN = 1
  - Parameters used around warmup boundaries when resetting timers or preloading the hardware timer.

- THRESHOLD_FIXEDPOINT_MULT = 65536.0
- THRESHOLD_DIVISOR = 1000
  - Used in computing `thresholdNs` (keeps original fixed-point style calculation but documents intent).

- TIME_OVERFLOW_BIAS = 50
- TIME_OVERFLOW_DIV = 100
  - Replace the literal `(lastOverflow + 50) / 100` used to increment `state_.time`.

- TCA0_PRELOAD_COUNT = 25570
  - Named constant for the hardware-specific preload value used with `setTCA0Count_()`.

- PPS_LOCK_LP_FACTOR = 16
- PPS_LOCK_DIFF_NS_LIMIT = 20
  - Values used by the PPS lock detection low-pass and thresholds.

- FILTER_CONST_MIN = 1
- FILTER_CONST_MAX = 1024
  - Bounds used when constraining `filterConst`.

- DIFF_NS_FILTER_THRESHOLD = 6500
- GAIN_LIMIT_BASE = 65535
- GAIN_LIMIT_EXTRA_MULT = 200
  - Used for the low-pass tic filter guard conditions.

- TIC_SCALE_FACTOR = 1000.0
- TIC_SCALE_FACTOR2 = 1000000.0
  - Factors used in polynomial tic linearization; named to indicate scaling and to reduce duplication.

- LONGTERM_INTERVAL_SEC = 300
- LONGTERM_3H_DIVISOR = 36
- LONGTERM_J_WRAP = 144
  - Constants for the long-term aggregation logic (300s intervals, 3h divisor, and wrap count).

- MAX_GPS_BYTES_PER_LOOP = 32 (type: uint8_t)
  - New project-wide constant added to `src/Constants.h`. It bounds how many serial bytes from the GPS are processed per main-loop iteration. It is now typed as `uint8_t` because it represents a small byte count. Tune this value in `src/Constants.h` if needed.

Notes
- The calculation-only constants were moved to `src/CalculationConstants.h` to keep them private to the controller implementation and avoid polluting the public `Constants.h` header.
- `src/Constants.h` remains the place for shared/public constants and types and now contains the GPS throttling constant `MAX_GPS_BYTES_PER_LOOP`.
- No behaviour change was introduced; the numeric values are unchanged except for the GPS drain handling which is now bounded to the configured constant.
