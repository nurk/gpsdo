/**
 * CalculationController.cpp — clean-room GPSDO discipline loop
 *
 * Design
 * ------
 * The fundamental observable is the TIC ADC reading: a ~1 ns/count measurement of the
 * phase difference between the GPS 1PPS and the OCXO 1PPS.  The 5 MHz counter (TCA0)
 * adds coarse microsecond resolution via `timerUs`, extending the capture range beyond
 * the ~1 µs TIC window.
 *
 * Total phase error each tick (ns):
 *   phaseNs = timerUs × 1000 + ticValue
 *
 * This is an ABSOLUTE phase measurement.  The previous implementation differentiated it
 * (diffNs = phaseNs[n] − phaseNs[n−1]) before filtering, which made every 1 µs step in
 * timerUs appear as a ±1000 ns spike in the error signal.  Using absolute phase avoids
 * that entirely — no timerUsJustStepped guard is needed.
 *
 * Loop (each PPS tick, after warmup)
 * -----------------------------------
 *   phaseNs        = timerUs × 1000 + ticValueCorrection          (absolute, ns)
 *   ticValueFiltered += (phaseNs × filterConst − ticValueFiltered) / filterConst  (IIR)
 *   error          = ticValueFiltered / filterConst − targetPhaseNs
 *   dacValue      += error × gain / (damping × timeConst)          (integrator)
 *   dacOut         = (dacValue + tempCorrection) / timeConst
 *
 * States
 * ------
 *   WARMUP    (time ≤ warmupTime) : DAC frozen; filter NOT updated; PI off.
 *   ACQUIRING (after warmup, !locked): filterConst = 1; PI runs.
 *   LOCKED    (lockPpsCounter ≥ threshold): filterConst = timeConst/filterDiv; PI runs.
 *
 * timerUs
 * -------
 * Accumulates coarse phase (µs).  Its absolute value does not matter to the PI loop;
 * only phaseNs = timerUs×1000 + ticValue matters, and that is well-behaved whether
 * timerUs is 0 or −300.  It is reset at the warmup boundary so the integrator starts clean.
 */

#include <CalculationController.h>
#include <CalculationConstants.h>
#include <Arduino.h>
#include <math.h> // NOLINT(*-deprecated-headers)

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CalculationController::CalculationController(const SetDacFn        setDac,
                                             const ReadTempFn      readTemp,
                                             const SaveStateFn     saveState,
                                             const SetTCA0CountFn  setTCA0Count)
    : setDac_(setDac), readTemp_(readTemp), saveState_(saveState), setTCA0Count_(setTCA0Count)
{
    state_.dacValue    = 0;
    state_.dacValueOut = 0;
    state_.dacOut      = 0;
    // Prime filter to ticOffset so pTerm starts at zero on the first PI tick.
    state_.ticValueFiltered          = static_cast<int32_t>(state_.ticOffset)
                                     * static_cast<int32_t>(state_.filterConst);
    state_.ticValueFilteredOld       = state_.ticValueFiltered;
    state_.ticValueFilteredForPpsLock = state_.ticValueFiltered;
}

// ---------------------------------------------------------------------------
// Public reset (hold → run transition)
// ---------------------------------------------------------------------------

void CalculationController::resetShortTermAccumulators() {
    state_.timerUs             = 0;
    state_.timerUsOld          = 0;
    state_.ticValueFiltered    = static_cast<int32_t>(state_.ticOffset)
                               * static_cast<int32_t>(state_.filterConst);
    state_.ticValueFilteredOld = state_.ticValueFiltered;
    // Reset lock-detection IIRs so they don't carry warmup-phase bias into the PI phase.
    // During warmup phaseNs = timerUs*1000 + ticValue can be −200000..0; with LP factor=16
    // the IIR accumulates a large negative value that keeps lockCtr at zero for hundreds of
    // seconds after the OCXO has actually converged.
    state_.diffNsForPpsLock              = lroundf(state_.ticValueCorrectionOffset)
                                         * PPS_LOCK_LP_FACTOR;
    state_.ticValueFilteredForPpsLock    = static_cast<int32_t>(state_.ticOffset)
                                         * PPS_LOCK_LP_FACTOR;
    state_.lockPpsCounter                = 0;
}

// ---------------------------------------------------------------------------
// Main entry — called once per PPS event
// ---------------------------------------------------------------------------

void CalculationController::calculate(const unsigned int  localTimerCounter,
                                      const int           localTicValue,
                                      const unsigned long lastOverflow,
                                      const bool          isRun,
                                      const uint16_t      warmupTime)
{
    preloadHeuristic(warmupTime);
    ticLinearization(localTicValue);
    updateTimersAndResets(localTimerCounter, lastOverflow, isRun, warmupTime);
    ppsLockDetection();
    determineFilterConstAndRescale(isRun);
    lowPassTicFilter(warmupTime);
    piLoop(isRun, warmupTime);
    tempCompensationAndDacOutput(isRun);
    updateLongTermState(isRun);
    updateSnapshots(localTimerCounter);

#ifdef DEBUG_CALCULATION
    // Compute derived display values
    const float dacVoltage      = static_cast<float>(state_.dacOut) / DAC_MAX_VALUE * 5.0f;
    const float filteredPhaseNs = static_cast<float>(state_.ticValueFiltered)
                                / static_cast<float>(state_.filterConst);
    const float targetPhaseNs   = state_.ticValueCorrectionOffset;

    // --- hardware inputs ---
    Serial2.print(F("time="));               Serial2.print(state_.time);
    Serial2.print(F(", delta="));            Serial2.print(state_.timerCounterDelta);
    Serial2.print(F(", timerUs="));          Serial2.print(state_.timerUs);
    Serial2.print(F(", ticValue="));         Serial2.print(localTicValue);

    // --- phase measurement (absolute, ns) ---
    Serial2.print(F(", phaseNs="));          Serial2.print(state_.diffNs);          // timerUs*1000 + ticCorr
    Serial2.print(F(", filteredPhaseNs="));  Serial2.print(filteredPhaseNs, 1);     // IIR output ÷ filterConst
    Serial2.print(F(", targetPhaseNs="));    Serial2.print(targetPhaseNs, 1);       // linearized ticOffset

    // --- filter / lock state ---
    Serial2.print(F(", filterConst="));      Serial2.print(state_.filterConst);
    Serial2.print(F(", locked="));           Serial2.print(state_.ppsLocked ? 1 : 0);
    Serial2.print(F(", lockCtr="));          Serial2.print(state_.lockPpsCounter);
    Serial2.print(F(", ticLpf="));           Serial2.print(state_.ticValueFilteredForPpsLock / PPS_LOCK_LP_FACTOR);
    Serial2.print(F(", phaseLpf="));         Serial2.print(state_.diffNsForPpsLock / PPS_LOCK_LP_FACTOR);
    Serial2.print(F(", phaseLpfChg="));      Serial2.print((state_.diffNsForPpsLock - state_.diffNsForPpsLockOld) / PPS_LOCK_LP_FACTOR);

    // --- PI loop ---
    Serial2.print(F(", pTerm="));            Serial2.print(state_.pTerm, 1);
    Serial2.print(F(", iTermLong="));        Serial2.print(state_.iTermLong);
    Serial2.print(F(", iTermRemain="));      Serial2.print(state_.iTermRemain, 3);

    // --- DAC output ---
    Serial2.print(F(", dacOut="));           Serial2.print(state_.dacOut);
    Serial2.print(F(", dacV="));             Serial2.println(dacVoltage, 4);
#endif
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void CalculationController::preloadHeuristic(const uint16_t warmupTime) const {
    if (state_.time < WARMUP_PRELOAD_MARGIN ||
        (state_.time > warmupTime - WARMUP_PRELOAD_MARGIN && state_.time < warmupTime)) {
        setTCA0Count_(TCA0_PRELOAD_COUNT);
    }
}

void CalculationController::ticLinearization(const int localTicValue) {
    state_.ticValue = localTicValue;
    state_.x1 = 1.0f - state_.x3 - state_.x2;

    // Polynomial linearization: maps raw ADC counts to ns via cubic correction.
    auto correct = [&](const float rawCounts) -> float {
        const float s = (rawCounts - state_.ticMin) / (state_.ticMax - state_.ticMin) * TIC_SCALE_FACTOR;
        return s * state_.x1
             + s * s * state_.x2     / TIC_SCALE_FACTOR
             + s * s * s * state_.x3 / TIC_SCALE_FACTOR2;
    };

    state_.ticValueCorrectionOffset = correct(static_cast<float>(state_.ticOffset));
    state_.ticValueCorrection        = correct(static_cast<float>(state_.ticValue));
}

void CalculationController::updateTimersAndResets(const unsigned int  localTimerCounter,
                                                   const unsigned long lastOverflow,
                                                   const bool          isRun,
                                                   const uint16_t      warmupTime)
{
    // --- counter delta, half-period wrap detection ---
    int32_t deltaCounter = static_cast<int32_t>(localTimerCounter)
                         - static_cast<int32_t>(state_.timerCounterValueOld);
    // A genuine wrap gives delta ≈ −49997; jitter gives delta = −1..−5.
    // The half-period threshold (25000) cleanly separates them.
    if (deltaCounter < -TIMER_COUNTER_HALF_PERIOD) {
        deltaCounter += TIMER_COUNTER_MODULO;
    }
    state_.timerCounterDelta = deltaCounter;

    // --- timerUs: coarse phase accumulator ---
    // Accumulates µs of phase difference from nominal 50000 counts/PPS.
    // The ticValue sub-term provides sub-µs interpolation, consistent with the original formula.
    state_.timerUs = state_.timerUs + TIMER_US_INCREMENT
                   - ((deltaCounter * TIMER_US_SCALE_FACTOR
                       + state_.ticValue - state_.ticValueOld)
                      + TIMER_US_BIAS) / TIMER_US_DIVISOR;

    // --- resets ---
    // All three cases call resetShortTermAccumulators() which zeros timerUs, timerUsOld,
    // ticValueFiltered, and ticValueFilteredOld — the complete set of state that must be
    // consistent with each other when the PI loop re-engages.
    const bool inWarmupWindow =
        (state_.time < WARMUP_RESET_THRESHOLD) ||
        (state_.time > warmupTime - WARMUP_RESET_MARGIN &&
         state_.time < warmupTime + WARMUP_RESET_MARGIN);

    if (inWarmupWindow) {
        resetShortTermAccumulators();
    }

    const long thresholdNs = static_cast<long>(
        static_cast<float>(state_.timeConst) * THRESHOLD_FIXEDPOINT_MULT
        / state_.gain / THRESHOLD_DIVISOR);
    if ((abs(state_.timerUs) - 2) > thresholdNs && isRun && state_.time > warmupTime) {
        resetShortTermAccumulators();
    }

    if (state_.ticValueOld == ADC_MAX_READING) {
        resetShortTermAccumulators();
    }

    // --- store absolute phaseNs in diffNs for debug log compatibility ---
    state_.diffNs = static_cast<int32_t>(state_.timerUs) * TIC_NS_PER_US
                  + lroundf(state_.ticValueCorrection);

    // --- time (seconds) ---
    state_.time = state_.time + (lastOverflow + TIME_OVERFLOW_BIAS) / TIME_OVERFLOW_DIV;

    if (state_.time - state_.timeOld > 1) {
        state_.missedPps++;
        state_.timeSinceMissedPps = 0;
    } else {
        state_.timeSinceMissedPps++;
    }
}

void CalculationController::ppsLockDetection() {
    // IIR on raw ticValue — tracks the TIC ADC mean, independent of timerUs.
    state_.ticValueFilteredForPpsLock =
        state_.ticValueFilteredForPpsLock
        + (state_.ticValue * PPS_LOCK_LP_FACTOR - state_.ticValueFilteredForPpsLock)
        / PPS_LOCK_LP_FACTOR;

    // IIR on absolute phaseNs — used to detect frequency stability (slow drift).
    // No timerUsJustStepped guard needed: absolute phase is smooth even when timerUs steps.
    state_.diffNsForPpsLock =
        state_.diffNsForPpsLock
        + (state_.diffNs * PPS_LOCK_LP_FACTOR - state_.diffNsForPpsLock)
        / PPS_LOCK_LP_FACTOR;

    state_.lockPpsCounter++;

    // Condition 1: TIC mean must be within lockPpsLimit counts of ticOffset.
    // ticLpf = ticValueFilteredForPpsLock / PPS_LOCK_LP_FACTOR ≈ mean ticValue.
    // This checks that the phase is centred on the target.
    if (abs(state_.ticValueFilteredForPpsLock / PPS_LOCK_LP_FACTOR - state_.ticOffset)
            > state_.lockPpsLimit) {
        state_.lockPpsCounter = 0;
    }

    // Condition 2: phase must not be drifting — check that consecutive phaseLpf values
    // are close. We track this by comparing diffNsForPpsLock to the previous tick's value.
    // A large difference means the OCXO is still slewing.
    // We store the previous phaseLpf in diffNsForPpsLockOld (reuse timerUsOld-style pattern).
    const int32_t phaseLpfChange = state_.diffNsForPpsLock - state_.diffNsForPpsLockOld;
    constexpr int32_t PHASE_DRIFT_LIMIT = 500 * PPS_LOCK_LP_FACTOR; // 500 ns/tick × 16
    if (abs(phaseLpfChange) > PHASE_DRIFT_LIMIT) {
        state_.lockPpsCounter = 0;
    }

    state_.ppsLocked = (state_.lockPpsCounter > state_.timeConst * state_.lockPpsFactor);
}

void CalculationController::determineFilterConstAndRescale(const bool isRun) {
    state_.filterConst = state_.timeConst / state_.filterDiv;
    state_.filterConst = constrain(state_.filterConst, FILTER_CONST_MIN, FILTER_CONST_MAX);
    if (!state_.ppsLocked || !isRun) {
        state_.filterConst = FILTER_CONST_MIN; // 1: direct assignment, no pre-filtering
    }

    if (state_.timeConst != state_.timeConstOld) {
        state_.dacValue = state_.dacValue / state_.timeConstOld * state_.timeConst;
    }

    if (state_.filterConst != state_.filterConstOld) {
        state_.ticValueFilteredOld = state_.ticValueFilteredOld
                                   / state_.filterConstOld * state_.filterConst;
        state_.ticValueFiltered    = state_.ticValueFiltered
                                   / state_.filterConstOld * state_.filterConst;
    }
}

void CalculationController::lowPassTicFilter(const uint16_t warmupTime) {
    // Block updates during warmup entirely.
    // With filterConst=1 the IIR is a direct assignment, so the filter tracks
    // phaseNs each tick.  During warmup timerUs drifts to −200..−300 µs, giving
    // phaseNs ≈ −200000 ns (rejected by outlier gate).  But for the ≈99 ticks where
    // |timerUs| < 100, phaseNs ≈ −600..−99000 ns — some of these pass the gate and
    // bias ticValueFiltered well below ticOffset before the PI loop starts.
    // Blocking during warmup eliminates this problem entirely.
    if (state_.time <= warmupTime) {
        return;
    }

    const int32_t phaseNs = static_cast<int32_t>(state_.timerUs) * TIC_NS_PER_US
                          + lroundf(state_.ticValueCorrection);

    // Outlier gate: skip if OCXO is more than 100 µs off (GPS glitch / missed PPS)
    constexpr int32_t OUTLIER_GATE_NS = 100000L;
    if (labs(phaseNs) > OUTLIER_GATE_NS) {
        return;
    }

    // IIR: ticValueFiltered tracks phaseNs × filterConst
    state_.ticValueFiltered = state_.ticValueFiltered
        + (phaseNs * static_cast<int32_t>(state_.filterConst)
           - state_.ticValueFiltered
           + static_cast<int32_t>(state_.filterConst) / 2)
        / static_cast<int32_t>(state_.filterConst);
}

void CalculationController::piLoop(const bool isRun, const uint16_t warmupTime) {
    if (!isRun || state_.time <= warmupTime) {
        return;
    }

    // Filtered phase in ns (ticValueFiltered is scaled by filterConst, so divide back)
    const float filteredPhaseNs = static_cast<float>(state_.ticValueFiltered)
                                / static_cast<float>(state_.filterConst);

    // Target phase in ns (linearized ticOffset)
    const float targetPhaseNs = state_.ticValueCorrectionOffset;

    // Proportional error
    state_.pTerm = (filteredPhaseNs - targetPhaseNs) * state_.gain;

    // Integrator step with sub-integer remainder preservation
    state_.iTerm       = state_.pTerm / state_.damping
                       / static_cast<float>(state_.timeConst)
                       + state_.iTermRemain;
    state_.iTermLong   = static_cast<long>(state_.iTerm);
    state_.iTermRemain = state_.iTerm - static_cast<float>(state_.iTermLong);

    // Step clamp: prevents a single bad sample from railing the DAC.
    state_.iTermLong = constrain(state_.iTermLong, -MAX_ITERM_STEP, MAX_ITERM_STEP);

    state_.dacValue += state_.iTermLong;
}

void CalculationController::tempCompensationAndDacOutput(const bool isRun) {
    state_.tempC      = readTemp_();
    state_.tempFilteredC += (state_.tempC - state_.tempFilteredC) / 100.0f;

    const long tempCorrection = computeTempCorrectionScaled();
    state_.dacValueOut = state_.dacValue + tempCorrection;

    const long dacMax = static_cast<long>(DAC_MAX_VALUE) * static_cast<long>(state_.timeConst);
    state_.dacValueOut = constrain(state_.dacValueOut, 0L, dacMax);

    uint16_t dacOut = 0;
    if (state_.timeConst != 0) {
        dacOut = static_cast<uint16_t>(
            constrain(lroundf(static_cast<float>(state_.dacValueOut)
                              / static_cast<float>(state_.timeConst)),
                      0L, static_cast<long>(DAC_MAX_VALUE)));
    }

    if (state_.holdValue > 0 && !isRun) {
        dacOut = state_.holdValue;
    }

    state_.dacOut = dacOut;
    setDac_(state_.dacOut);
}

void CalculationController::updateSnapshots(const unsigned int localTimerCounter) {
    state_.timerCounterValueOld  = localTimerCounter;
    state_.ticValueOld           = state_.ticValue;
    state_.ticValueCorrectionOld = state_.ticValueCorrection;
    state_.timerUsOld            = state_.timerUs;
    state_.timeConstOld          = state_.timeConst;
    state_.filterConstOld        = state_.filterConst;
    state_.timeOld               = state_.time;
    state_.ticValueFilteredOld   = state_.ticValueFiltered;
    state_.diffNsForPpsLockOld   = state_.diffNsForPpsLock;
}

long CalculationController::computeTempCorrectionScaled() const {
    const float delta  = state_.tempReferenceC - state_.tempFilteredC;
    const float scaled = delta * state_.tempCoefficientC * static_cast<float>(state_.timeConst);
    return static_cast<long>(scaled);
}

void CalculationController::updateLongTermState(const bool isRun) {
    longTermState_.sumTic  += state_.ticValue * 10;
    longTermState_.sumTemp += lroundf(state_.tempC * 10);
    longTermState_.sumDac  += state_.dacValueOut;
    longTermState_.i++;

    if (longTermState_.i != LONGTERM_INTERVAL_SEC) {
        return;
    }

    if (state_.time > 100 && longTermState_.restartFlag) {
        longTermState_.restarts++;
        longTermState_.restartFlag = false;
    }

    if (isRun) {
        longTermState_.ticArray[longTermState_.j] = longTermState_.sumTic  / longTermState_.i;
        longTermState_.dacArray[longTermState_.j] = longTermState_.sumDac  / longTermState_.i;
    } else {
        longTermState_.ticArray[longTermState_.j] = state_.ticValue;
        longTermState_.dacArray[longTermState_.j] = state_.dacValueOut;
    }
    longTermState_.tempArray[longTermState_.j] = longTermState_.sumTemp / longTermState_.i;

    longTermState_.sumTic2  += longTermState_.sumTic  / longTermState_.i;  longTermState_.sumTic  = 0;
    longTermState_.sumTemp2 += longTermState_.sumTemp / longTermState_.i;  longTermState_.sumTemp = 0;
    longTermState_.sumDac2  += longTermState_.sumDac  / longTermState_.i;  longTermState_.sumDac  = 0;
    longTermState_.i = 0;
    longTermState_.j++;

    if (longTermState_.j % LONGTERM_3H_DIVISOR != 0) {
        return;
    }

    if (longTermState_.j == LONGTERM_J_WRAP) longTermState_.j = 0;
    longTermState_.k++;
    if (longTermState_.k == LONGTERM_J_WRAP) longTermState_.k = 0;

    longTermState_.ticAverage3h  = longTermState_.sumTic2  / LONGTERM_3H_DIVISOR; longTermState_.sumTic2  = 0;
    longTermState_.tempAverage3h = longTermState_.sumTemp2 / LONGTERM_3H_DIVISOR; longTermState_.sumTemp2 = 0;
    longTermState_.dacAverage3h  = longTermState_.sumDac2  / LONGTERM_3H_DIVISOR; longTermState_.sumDac2  = 0;
    longTermState_.totalTime3h++;

    saveState_();
}
