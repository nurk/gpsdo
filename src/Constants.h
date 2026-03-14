#ifndef GPSDO_V1_0_CONSTANTS_H
#define GPSDO_V1_0_CONSTANTS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <Arduino.h>

enum OpMode {
    RUN,
    HOLD,
    WARMUP
};

constexpr uint16_t DAC_MAX_VALUE = 65535;
constexpr float DAC_VREF = 5.0f; // REF5050 reference voltage (V)
constexpr uint16_t WARMUP_TIME_DEFAULT = 600; // seconds

using SetWarmupTimeFn = void(*)(uint16_t seconds);
using SetDacFn = void(*)(uint16_t value);
using ReadTempFn = float(*)();
using ReadOCXOTempFn = float(*)();
using SaveStateFn = void(*)();
using ManuallySaveStateFn = void(*)();
using SetTCA0CountFn = void(*)(uint16_t count);
using SetOpModeFn = void(*)(OpMode mode, int32_t holdValue);

struct GpsData {
    bool isPositionValid = false;
    double latitude = 0.0;
    double longitude = 0.0;

    bool isSatellitesValid = false;
    uint32_t satellites = 0;

    bool isDateValid = false;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;

    bool isTimeValid = false;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t centisecond = 0;
};

struct ControlState {
    bool isFirstTic = true;             // true until the first PPS tick has been used to seed *Old snapshots

    uint16_t dacValue = DAC_MAX_VALUE / 2;
    float dacVoltage = static_cast<float>(DAC_MAX_VALUE) * 0.5f / static_cast<float>(DAC_MAX_VALUE) * DAC_VREF;
    int32_t holdValue = 0;

    int32_t timerCounterValueOld = 0;
    int32_t timerCounterValueReal = 0;
    int32_t timerCounterError = 0;

    int32_t time = 0;
    int32_t timeOld = 0;

    int32_t missedPpsCounter = 0;
    int32_t timeSinceMissedPps = 0;

    int32_t ticValue = 0;
    int32_t ticValueOld = 0;
    double ticValueCorrection = 0.0; // linearized tic (raw)
    double ticValueCorrectionOld = 0.0;
    double ticValueCorrectionOffset = 0.0; // linearized value at ticOffset (zero reference)
    double ticCorrectedNetValue = 0.0; // ticValueCorrection - ticValueCorrectionOffset
    double ticCorrectedNetValueFiltered = 0.0;
    double ticFrequencyError = 0.0;
    bool ticFilterSeeded = false;       // true after the EMA has been seeded with the first real measurement
    int32_t ticFilterConst = 16;
    double ticDelta = 0.0;

    // --- PI loop state ---
    double iAccumulator = DAC_MAX_VALUE / 2.0; // integrator state (DAC counts); initialised to mid-scale
    double iAccumulatorLast = DAC_MAX_VALUE / 2.0; // snapshot of iAccumulator from previous tick (for drift guard)
    double iRemainder = 0.0;                   // fractional carry-forward to avoid truncation drift
    int32_t timeConst = 32;                    // loop time constant in seconds
    double gain = 12.0;                        // DAC counts per linearised TIC count (EFC sensitivity)
    double damping = 3.0;                      // P/I ratio — higher = more damped, slower pull-in
    double pTerm = 0.0;                        // proportional term (DAC counts)

    // --- DAC safety limits ---
    uint16_t dacMinValue = 0;
    uint16_t dacMaxValue = DAC_MAX_VALUE;

    // --- PPS Locked ---
    bool ppsLocked = false;
    int32_t ppsLockCount = 0;  // consecutive seconds within LOCK_THRESHOLD; lock declared at 2 × ticFilterConst

    double ticOffset = 500.0; // expected centre of TIC range (counts)
    // Polynomial coefficients for TIC linearization.
    // The polynomial is evaluated on a normalised input x = (tic - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000
    // using Horner form:  x*(a1 + x*(a2 + x*a3))
    // x1 is derived: x1 = 1 - x2 - x3  (ensures unity gain at full scale)
    // x2 and x3 are the quadratic and cubic correction terms.
    // Pre-scale x2 by 1/1000 and x3 by 1/100000 when storing so no runtime division is needed.
    double x2Coefficient = 1.0e-4; // quadratic term  (= 0.1 / 1000)
    double x3Coefficient = 3.0e-7; // cubic term      (= 0.03 / 100000)
};

constexpr int32_t COUNTS_PER_PPS = 5000000;
constexpr int32_t MODULO = 50000;

constexpr double TIC_MIN = 12.0;
constexpr double TIC_MAX = 1012.0;

// Lock detection thresholds (in linearised TIC counts, same units as ticCorrectedNetValueFiltered)
// Lock is declared after LOCK_COUNT_THRESHOLD consecutive seconds below LOCK_THRESHOLD.
// Unlock occurs immediately when the filtered error exceeds UNLOCK_THRESHOLD (hysteresis).
constexpr double LOCK_THRESHOLD   = 50.0;   // filtered phase error must stay within ±50 counts to declare lock
constexpr double UNLOCK_THRESHOLD = 100.0;  // filtered phase error must exceed ±100 counts to declare unlock

// Maximum absolute iAccumulator change per tick (DAC counts) that is allowed while
// counting toward lock.  While the integrator is still pulling in (e.g. 5+ counts/tick)
// the loop has not converged and a lock declaration would be premature.
// At the settled setpoint the I-step should be < 1 count/tick.
constexpr double LOCK_INTEGRATOR_DRIFT_MAX = 2.0;

// Maximum P-term contribution in DAC counts per tick.
// The raw TIC sawtooth spans ~500 counts/s × gain 12 = ~6000 counts, which would
// slam the DAC on every wrap. Clamping limits the kick while still providing
// meaningful frequency-error damping.
// Maximum P-term contribution in DAC counts per tick.
// With ticDelta as the P-term source (rate of change of linearised phase error):
//   - Sawtooth ramp at ~170 counts/s drift × gain 12 = ~2040 counts  → real signal, should pass
//   - Sawtooth wrap spike at ~500–800 counts  × gain 12 = ~6000–9600 → should be clamped
// A clamp of 2000 passes the ramp signal almost fully and caps wrap spikes cleanly.
// (When ticFrequencyError was the source, the coarseFreqError term inflated every tick
//  by ±200×timerCounterError, making 2000 fire constantly. With ticDelta that problem is gone.)
constexpr double PTERM_MAX_COUNTS = 2000.0;
#endif //GPSDO_V1_0_CONSTANTS_H
