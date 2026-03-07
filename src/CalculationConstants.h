#ifndef CALCULATION_CONSTANTS_H
#define CALCULATION_CONSTANTS_H

#include <Arduino.h>

// Constants used only by CalculationController implementation.
// These are intentionally separate from the public `Constants.h` to
// keep calculation-only configuration private to the implementation.

// ADC / TIC
constexpr int ADC_MAX_READING = 1023; // 10-bit ADC max value for TIC readings

// Timer / TIC update math
constexpr long TIMER_US_INCREMENT = 50000L;
constexpr int TIMER_US_SCALE_FACTOR = 200;
constexpr long TIMER_US_BIAS = 50000500L;
constexpr int TIMER_US_DIVISOR = 1000;
constexpr int TIC_NS_PER_US = 1000; // nanoseconds per microsecond

// Warmup margins used around the configured warmup time (seconds)
constexpr uint16_t WARMUP_PRELOAD_MARGIN = 2;
constexpr uint16_t WARMUP_RESET_THRESHOLD = 3;
constexpr uint16_t WARMUP_RESET_MARGIN = 1;

// Threshold scaling constants used when computing thresholdNs
constexpr float THRESHOLD_FIXEDPOINT_MULT = 65536.0f;
constexpr int THRESHOLD_DIVISOR = 1000;

// Time overflow conversion used when updating `time`
constexpr int TIME_OVERFLOW_BIAS = 50;
constexpr int TIME_OVERFLOW_DIV = 100;

// TCA0 counter range: counts 0..TCA0_COUNTER_MODULO-1 (PER=49999, driven by 5 MHz events)
// HALF_PERIOD is used for genuine wrap detection: any raw delta < -HALF_PERIOD must be a wrap,
// not jitter, because normal PPS-to-PPS counter advance is only 2–7 counts.
constexpr int32_t TIMER_COUNTER_MODULO = 50000;
constexpr int32_t TIMER_COUNTER_HALF_PERIOD = TIMER_COUNTER_MODULO / 2; // 25000

// TCA0 preload count heuristic (hardware timer value)
constexpr uint16_t TCA0_PRELOAD_COUNT = 25570;

// PPS lock detection low-pass / limits
constexpr int PPS_LOCK_LP_FACTOR = 16;
// During initial acquisition the oscillator may be several hundred ns/s off at the midpoint
// DAC value.  A limit of 20 ns was unreachably tight and created a deadlock: the PI loop
// would not run until PPS lock was achieved, but PPS lock could not be achieved until the
// PI loop had corrected the frequency.  500 ns allows lock to be declared while still pulling
// in, so the filter constant can ramp up and the loop can converge cleanly.
constexpr int PPS_LOCK_DIFF_NS_LIMIT = 500;

// Filter constants bounds
constexpr int FILTER_CONST_MIN = 1;
constexpr int FILTER_CONST_MAX = 1024;

// Low-pass tic filter thresholds and gain limits
constexpr int DIFF_NS_FILTER_THRESHOLD = 6500;
constexpr long GAIN_LIMIT_BASE = 65535L;
constexpr int GAIN_LIMIT_EXTRA_MULT = 200; // multiplier applied to gain in the limit calculation

// Polynomial tic scaling factors
constexpr float TIC_SCALE_FACTOR = 1000.0f;
constexpr float TIC_SCALE_FACTOR2 = 1000000.0f;

// Long-term aggregation constants
constexpr int LONGTERM_INTERVAL_SEC = 300; // aggregate interval in seconds
constexpr int LONGTERM_3H_DIVISOR = 36; // number of 5-min intervals in 3h (300s * 36 = 3h)
constexpr int LONGTERM_J_WRAP = 144; // wrap value used for 12h indexing

#endif // CALCULATION_CONSTANTS_H
