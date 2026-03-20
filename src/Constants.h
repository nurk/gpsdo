#ifndef GPSDO_V1_0_CONSTANTS_H
#define GPSDO_V1_0_CONSTANTS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <Arduino.h>

enum OpMode {
    RUN,
    HOLD,
    WARMUP
};

constexpr uint16_t DAC_MAX_VALUE       = 65535;
constexpr float DAC_VREF               = 5.0f; // REF5050 voltage reference for the DAC (V)
constexpr float ADC_VREF               = 1.1f; // ATmega4808 internal reference for the TIC ADC (V)
constexpr uint16_t WARMUP_TIME_DEFAULT = 600; // seconds

// Best-guess EFC starting point for the next power-on, in DAC counts.
// Seeding close to the true on-frequency value avoids the multi-hour pull-in
// from mid-scale (32767) seen when the OCXO EFC setpoint is far from 2.5 V.
// Update this after each long settled run — use the iAccumulator value once
// the loop has been locked and stable for >1 hour (read from the log).
// Observed settled values (open-air, ~30.6 °C OCXO):
//   run9:  22787  (1.739 V)
//   run10: ~22980  (1.754 V)  — settled after 4356 s in RUN, 3592 s continuously locked
constexpr uint16_t DAC_INITIAL_VALUE = 22880; // midpoint of run9/run10 settled values

struct GpsData {
    bool isPositionValid = false;
    double latitude      = 0.0;
    double longitude     = 0.0;

    bool isSatellitesValid = false;
    uint32_t satellites    = 0;

    bool isDateValid = false;
    uint16_t year    = 0;
    uint8_t month    = 0;
    uint8_t day      = 0;

    bool isTimeValid    = false;
    uint8_t hour        = 0;
    uint8_t minute      = 0;
    uint8_t second      = 0;
    uint8_t centisecond = 0;
};

struct ControlState {
    bool isFirstTic = true; // true until the first PPS tick has been used to seed *Old snapshots

    uint16_t dacValue = DAC_INITIAL_VALUE;
    float dacVoltage  = static_cast<float>(DAC_INITIAL_VALUE) / static_cast<float>(DAC_MAX_VALUE) * DAC_VREF;
    int32_t holdValue = 0;

    int32_t timerCounterValueOld  = 0;
    int32_t timerCounterValueReal = 0;
    int32_t timerCounterError     = 0;

    int32_t time           = 0;
    int32_t timeOld        = 0;
    int32_t storeStateTime = 0;

    int32_t missedPpsCounter   = 0;
    int32_t timeSinceMissedPps = 0;

    int32_t ticValue                    = 0;
    int32_t ticValueOld                 = 0;
    double ticValueCorrection           = 0.0; // linearized tic (raw)
    double ticValueCorrectionOld        = 0.0;
    double ticValueCorrectionOffset     = 0.0; // linearized value at ticOffset (zero reference)
    double ticCorrectedNetValue         = 0.0; // ticValueCorrection - ticValueCorrectionOffset
    double ticCorrectedNetValueFiltered = 0.0;
    double ticFrequencyError            = 0.0;
    bool ticFilterSeeded                = false; // true after the EMA has been seeded with the first real measurement
    int32_t ticFilterConst              = 16;
    double ticDelta                     = 0.0;

    // --- PI loop state ---
    double iAccumulator = DAC_INITIAL_VALUE; // integrator state (DAC counts); seeded from DAC_INITIAL_VALUE
    double iRemainder   = 0.0; // fractional carry-forward to avoid truncation drift
    int32_t timeConst   = 32; // loop time constant in seconds
    double gain         = 12.0; // DAC counts per linearised TIC count (EFC sensitivity)
    double damping      = 3.0; // P/I ratio — higher = more damped, slower pull-in
    double pTerm        = 0.0; // proportional term (DAC counts)

    // --- DAC safety limits ---
    uint16_t dacMinValue = 0;
    uint16_t dacMaxValue = DAC_MAX_VALUE;

    // --- Coarse frequency trim ---
    // A slow secondary integrator on timerCounterError that corrects residual
    // frequency offsets the fine TIC loop cannot see (e.g. OCXO running fast/slow
    // by a fraction of a coarse counter tick, visible as a persistent non-zero mean
    // timerCounterError). Accumulates timerCounterError each tick and applies a
    // tiny trim to iAccumulator every coarseTrimPeriod seconds.
    // coarseTrimGain: DAC counts added to iAccumulator per unit of accumulated
    // timerCounterError per coarseTrimPeriod seconds. Keep very small — this is a
    // slow outer loop. Default 0.5 means one coarse tick of persistent error
    // contributes 0.5 DAC counts per trim period.
    double coarseErrorAccumulator = 0.0; // running sum of timerCounterError
    double coarseTrimGain         = 0.5; // DAC counts per accumulated coarse count per period
    int32_t coarseTrimPeriod      = 64; // seconds between coarse trim steps (must be > timeConst)
    double lastCoarseTrim         = 0.0; // most recent coarse trim applied (logged each PPS; 0 on non-trim ticks)
    // When a coarse trim fires to pull the accumulator away from a rail, the I-term
    // is suppressed for coarseTrimPeriod ticks so the trim has a full period to work
    // uncontested before the I-term can drain it back.  Counts down each tick.
    int32_t iTermSuppressCount    = 0;

    // --- PPS Locked ---
    bool ppsLocked       = false;
    int32_t ppsLockCount = 0; // consecutive seconds within LOCK_THRESHOLD; lock declared at 2 × ticFilterConst

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

struct EEPROMState {
    // Fields stored to EEPROM — keep this struct stable; bump kMagic version byte
    // in ExternalEEPROMController.h whenever the layout changes.
    uint16_t dacValue; // last settled DAC output value
    double iAccumulator; // integrator state (DAC counts)
};

// DEFAULT_EEPROM_STATE represents a cold-start fallback (no valid EEPROM bank found).
// dacValue and iAccumulator are both seeded from DAC_INITIAL_VALUE so the loop starts
// from the best-known EFC setpoint rather than 0 or mid-scale.
constexpr EEPROMState DEFAULT_EEPROM_STATE = {
    DAC_INITIAL_VALUE, // dacValue
    DAC_INITIAL_VALUE, // iAccumulator — cold-start seed, matches dacValue
};

constexpr int32_t COUNTS_PER_PPS = 5000000;
constexpr int32_t MODULO         = 50000;

constexpr double TIC_MIN = 12.0;
constexpr double TIC_MAX = 1012.0;

// Lock detection thresholds (in linearised TIC counts, same units as ticCorrectedNetValueFiltered)
// Lock is declared after 2 × ticFilterConst consecutive seconds below LOCK_THRESHOLD.
// Unlock occurs immediately when the filtered error exceeds UNLOCK_THRESHOLD (hysteresis).
constexpr double LOCK_THRESHOLD   = 50.0; // filtered phase error must stay within ±50 counts to declare lock
constexpr double UNLOCK_THRESHOLD = 100.0; // filtered phase error must exceed ±100 counts to declare unlock


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

// Maximum plausible timerCounterError magnitude for coarse accumulator input.
// Normal operation: ±1–5 counts (±200–1000 ns/s).
// Values beyond this are hardware glitches (e.g. spurious overflow-counter
// increments from a noisy PPS edge) and must not be accumulated — one poisoned
// tick multiplied by coarseTrimGain and applied 64 s later would crash the DAC.
// 50 counts = 10 µs/s = 10 ppm — impossibly large for a disciplined OCXO;
// anything beyond this is unambiguously a glitch.
constexpr int32_t COARSE_ERROR_SANITY_LIMIT = 50;


using SetWarmupTimeFn     = void(*)(uint16_t seconds);
using SetDacFn            = void(*)(uint16_t value);
using ReadTempFn          = float(*)();
using ReadOCXOTempFn      = float(*)();
using SaveStateFn         = void(*)(const EEPROMState& eepromState);
using ManuallySaveStateFn = void(*)();
using SetTCA0CountFn      = void(*)(uint16_t count);
using SetOpModeFn         = void(*)(OpMode mode, int32_t holdValue);
#endif //GPSDO_V1_0_CONSTANTS_H
