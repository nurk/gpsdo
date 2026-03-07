#include <CalculationController.h>
#include <CalculationConstants.h>
#include <Arduino.h>
#include <math.h> // NOLINT(*-deprecated-headers)

CalculationController::CalculationController(const SetDacFn setDac,
                                             const ReadTempFn readTemp,
                                             const SaveStateFn saveState,
                                             const SetTCA0CountFn setTCA0Count)
    : setDac_(setDac),
      readTemp_(readTemp),
      saveState_(saveState),
      setTCA0Count_(setTCA0Count) {
    // Initialize short-term filtered TIC state to the expected nominal value so the
    // PI loop does not immediately push the DAC to zero on startup.
    state_.ticValueFiltered = static_cast<int32_t>(state_.ticOffset) * static_cast<int32_t>(state_.filterConst);
    state_.ticValueFilteredOld = state_.ticValueFiltered;
    state_.ticValueFilteredForPpsLock = state_.ticValueFiltered;
    // Ensure dacValueOut/dacValue start in a consistent state
    state_.dacValue = 0;
    state_.dacValueOut = 0;
    state_.dacOut = 0;
}

long CalculationController::computeTempCorrectionScaled() const {
    const float delta = state_.tempReferenceC - state_.tempFilteredC;
    const float scaled = delta * state_.tempCoefficientC * static_cast<float>(state_.timeConst);
    return static_cast<long>(scaled);
}

void CalculationController::resetShortTermAccumulators() {
    state_.timerUs = 0;
    state_.timerUsOld = 0;
    state_.ticValueFilteredOld = state_.ticOffset * state_.filterConst;
    state_.ticValueFiltered = state_.ticOffset * state_.filterConst;
    state_.ticValueFilteredForPpsLock = state_.ticValueFiltered;
}

void CalculationController::calculate(const unsigned int localTimerCounter,
                                      const int localTicValue,
                                      const unsigned long lastOverflow,
                                      const bool isRun,
                                      const uint16_t warmupTime) {
    // Keep original order but delegate to smaller helpers for clarity.
    preloadHeuristic(warmupTime);
    ticLinearization(localTicValue);
    updateTimersAndResets(localTimerCounter, lastOverflow, isRun, warmupTime);
    ppsLockDetection();
    determineFilterConstAndRescale(isRun);
    lowPassTicFilter();
    piLoop(isRun, warmupTime);
    tempCompensationAndDacOutput(isRun);
    updateLongTermState(isRun);
    updateSnapshots(localTimerCounter);

#ifdef DEBUG_CALCULATION

    const float dacVoltage = static_cast<float>(state_.dacOut) / DAC_MAX_VALUE * 5.0f;

    Serial2.print(F("time="));
    Serial2.print(state_.time);
    Serial2.print(F(", timerCounter="));
    Serial2.print(localTimerCounter);
    Serial2.print(F(", tic="));
    Serial2.print(localTicValue);
    Serial2.print(F(", delta="));
    Serial2.print(state_.timerCounterDelta);
    Serial2.print(F(", timerUs="));
    Serial2.print(state_.timerUs);
    Serial2.print(F(", diffNs="));
    Serial2.print(state_.diffNs);
    Serial2.print(F(", dacV="));
    Serial2.print(dacVoltage, 4);
    Serial2.print(F(", dacOut="));
    Serial2.print(state_.dacOut);

    Serial2.print(F(", lockPpsCounter="));
    Serial2.print(state_.lockPpsCounter);
    Serial2.print(F(", ticValueFilteredForPpsLock="));
    Serial2.print(state_.ticValueFilteredForPpsLock);
    Serial2.print(F(", diffNsForPpsLock="));
    Serial2.print(state_.diffNsForPpsLock);

    Serial2.print(F(", pTerm="));
    Serial2.print(state_.pTerm);
    Serial2.print(F(", iTerm="));
    Serial2.print(state_.iTerm);
    Serial2.print(F(", iTermLong="));
    Serial2.print(state_.iTermLong);
    Serial2.print(F(", iTermRemain="));
    Serial2.println(state_.iTermRemain);
#endif
}

void CalculationController::preloadHeuristic(const uint16_t warmupTime) const {
    if (state_.time < WARMUP_PRELOAD_MARGIN || (state_.time > warmupTime - WARMUP_PRELOAD_MARGIN && state_.time <
        warmupTime)) {
        setTCA0Count_(TCA0_PRELOAD_COUNT); // some heuristic; requires access to hardware timer
    }
}

void CalculationController::ticLinearization(const int localTicValue) {
    state_.ticValue = localTicValue;
    state_.x1 = 1.0f - state_.x3 - state_.x2;
    float ticScaled = (static_cast<float>(state_.ticOffset) - state_.ticMin) / (state_.ticMax - state_.ticMin) *
        TIC_SCALE_FACTOR;
    state_.ticValueCorrectionOffset = ticScaled * state_.x1 + ticScaled * ticScaled * state_.x2 / TIC_SCALE_FACTOR +
        ticScaled * ticScaled * ticScaled * state_.x3 / TIC_SCALE_FACTOR2;

    ticScaled = (static_cast<float>(state_.ticValue) - state_.ticMin) / (state_.ticMax - state_.ticMin) *
        TIC_SCALE_FACTOR;
    state_.ticValueCorrection = ticScaled * state_.x1 + ticScaled * ticScaled * state_.x2 / TIC_SCALE_FACTOR + ticScaled
        *
        ticScaled * ticScaled * state_.x3 / TIC_SCALE_FACTOR2;
}

void CalculationController::updateTimersAndResets(const unsigned int localTimerCounter,
                                                  const unsigned long lastOverflow,
                                                  const bool isRun,
                                                  const uint16_t warmupTime) {
    // --- counter delta, half-period wrap detection ---
    int32_t deltaCounter = static_cast<int32_t>(localTimerCounter)
        - static_cast<int32_t>(state_.timerCounterValueOld);
    // A genuine wrap gives delta ≈ −49997; jitter gives delta = −1..−5.
    // The half-period threshold (25000) cleanly separates them.
    if (deltaCounter < -TIMER_COUNTER_HALF_PERIOD) {
        deltaCounter += TIMER_COUNTER_MODULO;
    }
    state_.timerCounterDelta = deltaCounter;

    state_.timerUs = state_.timerUs + TIMER_US_INCREMENT - (((deltaCounter) *
        TIMER_US_SCALE_FACTOR +
        state_.ticValue - state_.ticValueOld) + TIMER_US_BIAS) / TIMER_US_DIVISOR;

    // reset in warmup
    if (state_.time < WARMUP_RESET_THRESHOLD || (state_.time > warmupTime - WARMUP_RESET_MARGIN && state_.time <
        warmupTime + WARMUP_RESET_MARGIN)) {
        resetShortTermAccumulators();
    }

    const long thresholdNs = static_cast<long>(static_cast<float>(state_.timeConst) * THRESHOLD_FIXEDPOINT_MULT / state_
        .gain /
        THRESHOLD_DIVISOR);
    if ((abs(state_.timerUs) - 2) > thresholdNs && isRun && state_.time > warmupTime) {
        state_.timerUs = 0;
        state_.timerUsOld = 0;
        state_.ticValueFilteredOld = state_.ticOffset * state_.filterConst;
    }

    if (state_.ticValueOld == ADC_MAX_READING) {
        state_.timerUs = 0;
        state_.timerUsOld = 0;
        state_.ticValueFilteredOld = state_.ticOffset * state_.filterConst;
    }

    // diff_ns
    state_.diffNs = (state_.timerUs - state_.timerUsOld) * TIC_NS_PER_US + lroundf(
        state_.ticValueCorrection - state_.ticValueCorrectionOld);

    // time
    state_.time = state_.time + (lastOverflow + TIME_OVERFLOW_BIAS) / TIME_OVERFLOW_DIV;

    // missed PPS
    if (state_.time - state_.timeOld > 1) {
        state_.missedPps = state_.missedPps + 1;
        state_.timeSinceMissedPps = 0;
    }
    else {
        state_.timeSinceMissedPps = state_.timeSinceMissedPps + 1;
    }
}

void CalculationController::ppsLockDetection() {
    // PPS lock detection low-pass
    state_.ticValueFilteredForPpsLock = state_.ticValueFilteredForPpsLock + (state_.ticValue * PPS_LOCK_LP_FACTOR -
        state_.ticValueFilteredForPpsLock) / PPS_LOCK_LP_FACTOR;
    state_.diffNsForPpsLock = state_.diffNsForPpsLock + (state_.diffNs * PPS_LOCK_LP_FACTOR - state_.diffNsForPpsLock) /
        PPS_LOCK_LP_FACTOR;

    state_.lockPpsCounter = state_.lockPpsCounter + 1;

    if (abs(state_.ticValueFilteredForPpsLock / PPS_LOCK_LP_FACTOR - state_.ticOffset) > state_.lockPpsLimit) {
        state_.lockPpsCounter = 0;
    }

    if (abs(state_.diffNsForPpsLock / PPS_LOCK_LP_FACTOR) > PPS_LOCK_DIFF_NS_LIMIT) {
        state_.lockPpsCounter = 0;
    }

    state_.ppsLocked = (state_.lockPpsCounter > state_.timeConst * state_.lockPpsFactor);
}

void CalculationController::determineFilterConstAndRescale(const bool isRun) {
    // Determine filter_const
    state_.filterConst = state_.timeConst / state_.filterDiv;
    state_.filterConst = constrain(state_.filterConst, FILTER_CONST_MIN, FILTER_CONST_MAX);
    if (!state_.ppsLocked || !isRun) {
        state_.filterConst = FILTER_CONST_MIN;
    }

    // rescale when time_const changed
    if (state_.timeConst != state_.timeConstOld) {
        state_.dacValue = state_.dacValue / state_.timeConstOld * state_.timeConst;
    }

    if (state_.filterConst != state_.filterConstOld) {
        state_.ticValueFilteredOld = state_.ticValueFilteredOld / state_.filterConstOld * state_.filterConst;
        state_.ticValueFiltered = state_.ticValueFiltered / state_.filterConstOld * state_.filterConst;
    }
}

void CalculationController::lowPassTicFilter() {
    // Low-pass TIC filtered
    // Note: the legacy code in archive/OriginalCode.cpp had a parentheses bug here:
    //    if ( abs(diff_ns <6500) ) // <-- this evaluates "diff_ns < 6500" first and thus always passes
    // which effectively disabled the outlier rejection and could drive the integrator with spurious values.
    // Use a strict numeric comparison and an additional measurement sanity check to protect the integrator.
    if (abs(state_.diffNs) < DIFF_NS_FILTER_THRESHOLD) {
        if (abs(static_cast<long>(state_.diffNs * state_.gain)) < static_cast<long>(GAIN_LIMIT_BASE +
            GAIN_LIMIT_EXTRA_MULT * state_.gain)) {
            // Guard against extremely large instantaneous TIC measurements (ns) slipping through
            const long measurement = static_cast<long>(state_.timerUs) * TIC_NS_PER_US + static_cast<long>(state_.
                ticValue);
            // Don't accept absurd single-sample measurements (e.g. caused by wrap/overflow or transient capture errors)
            constexpr long MEASUREMENT_NS_MAX = 100000L; // 100us in ns units
            if (labs(measurement) < MEASUREMENT_NS_MAX) {
                state_.ticValueFiltered = state_.ticValueFiltered + ((measurement) * state_.filterConst - state_.
                    ticValueFiltered + (state_.filterConst / 2)) / state_.filterConst;
            }
            else {
                // Skip update if the single-sample measurement is absurd; leave filtered value unchanged.
            }
        }
    }
}

void CalculationController::piLoop(const bool isRun, const uint16_t warmupTime) {
    // PI loop
    if (state_.time > warmupTime && isRun) {
        const auto TICValueFiltered_f = static_cast<float>(state_.ticValueFiltered);
        const auto TICOffset_f = static_cast<float>(state_.ticOffset);
        const auto filterConst_f = static_cast<float>(state_.filterConst);
        state_.pTerm = (TICValueFiltered_f - TICOffset_f * filterConst_f) / filterConst_f * state_.gain;
        state_.iTerm = state_.pTerm / state_.damping / static_cast<float>(state_.timeConst) + state_.iTermRemain;
        state_.iTermLong = static_cast<long>(state_.iTerm);
        state_.iTermRemain = state_.iTerm - static_cast<float>(state_.iTermLong);
        // Clamp the integrator step to avoid runaway due to transient bad samples.
        // The clamp is intentionally large so normal behaviour is unaffected but it prevents big jumps
        // observed when spurious TIC values slip through.
        constexpr long MAX_ITERM_STEP = 50000L;
        state_.iTermLong = constrain(state_.iTermLong, -MAX_ITERM_STEP, MAX_ITERM_STEP);
        state_.dacValue += state_.iTermLong;
    }
}

void CalculationController::tempCompensationAndDacOutput(const bool isRun) {
    // Temp compensation
    state_.tempC = readTemp_();
    state_.tempFilteredC += (state_.tempC - state_.tempFilteredC) / 100.0f;

    const long tempCorrectionScaled = computeTempCorrectionScaled();

    state_.dacValueOut = state_.dacValue + tempCorrectionScaled;

    if (state_.dacValueOut < 0) state_.dacValueOut = 0;
    if (state_.dacValueOut > static_cast<long>(DAC_MAX_VALUE) * state_.timeConst)
        state_.dacValueOut =
            static_cast<long>(DAC_MAX_VALUE) * state_.timeConst;

    uint16_t dacOut = 0;
    if (state_.timeConst != 0) {
        dacOut = lroundf(static_cast<float>(state_.dacValueOut) / static_cast<float>(state_.timeConst));
    }
    dacOut = constrain(dacOut, 0L, static_cast<long>(DAC_MAX_VALUE));

    if (state_.holdValue > 0 && !isRun) {
        dacOut = state_.holdValue;
    }

    state_.dacOut = dacOut;
    // call DAC write via callback
    setDac_(state_.dacOut);
}

void CalculationController::updateSnapshots(const unsigned int localTimerCounter) {
    // Save snapshots
    state_.timerCounterValueOld = localTimerCounter;
    state_.ticValueOld = state_.ticValue;
    state_.ticValueCorrectionOld = state_.ticValueCorrection;
    state_.timerUsOld = state_.timerUs;
    state_.timeConstOld = state_.timeConst;
    state_.filterConstOld = state_.filterConst;
    state_.timeOld = state_.time;
    state_.ticValueFilteredOld = state_.ticValueFiltered;
}

void CalculationController::updateLongTermState(const bool isRun) {
    longTermState_.sumTic += state_.ticValue * 10;
    longTermState_.sumTemp += lroundf(state_.tempC * 10);
    longTermState_.sumDac += state_.dacValueOut;

    longTermState_.i += 1;
    // every 300 seconds
    if (longTermState_.i == LONGTERM_INTERVAL_SEC) {
        if (state_.time > 100 && longTermState_.restartFlag == true) {
            longTermState_.restarts += 1;
            longTermState_.restartFlag = false;
        }

        if (isRun) {
            longTermState_.ticArray[longTermState_.j] = longTermState_.sumTic / longTermState_.i;
            longTermState_.dacArray[longTermState_.j] = longTermState_.sumDac / longTermState_.i;
        }
        else {
            longTermState_.ticArray[longTermState_.j] = state_.ticValue;
            longTermState_.dacArray[longTermState_.j] = state_.dacValueOut;
        }
        longTermState_.tempArray[longTermState_.j] = longTermState_.sumTemp / longTermState_.i;

        longTermState_.sumTic2 += longTermState_.sumTic / longTermState_.i;
        longTermState_.sumTic = 0;
        longTermState_.sumTemp2 += longTermState_.sumTemp / longTermState_.i;
        longTermState_.sumTemp = 0;
        longTermState_.sumDac2 += longTermState_.sumDac / longTermState_.i;
        longTermState_.sumDac = 0;

        longTermState_.i = 0;
        longTermState_.j += 1;
        // every 3 hours
        if (longTermState_.j % LONGTERM_3H_DIVISOR == 0) {
            // every 12 hours
            if (longTermState_.j == LONGTERM_J_WRAP) {
                longTermState_.j = 0;
            }
            // every 18 days
            longTermState_.k += 1;
            if (longTermState_.k == LONGTERM_J_WRAP) {
                longTermState_.k = 0;
            }

            longTermState_.ticAverage3h = longTermState_.sumTic2 / LONGTERM_3H_DIVISOR;
            longTermState_.sumTic2 = 0;

            longTermState_.tempAverage3h = longTermState_.sumTemp2 / LONGTERM_3H_DIVISOR;
            longTermState_.sumTemp2 = 0;

            longTermState_.dacAverage3h = longTermState_.sumDac2 / LONGTERM_3H_DIVISOR;
            longTermState_.sumDac2 = 0;

            longTermState_.totalTime3h += 1;

            // given there are 8 banks in the EEPROM, and it is an external one with higher endurance,
            // we save every 3 hours
            saveState_();
        }
    }
}
