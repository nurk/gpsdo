#include <CalculationController.h>
#include <Arduino.h>

CalculationController::CalculationController(const SetDacFn setDac,
                                             const ReadTempFn readTemp,
                                             const SaveStateFn saveState,
                                             const SetTCA0CountFn setTCA0Count)
    : setDac_(setDac),
      readTemp_(readTemp),
      saveState_(saveState),
      setTCA0Count_(setTCA0Count) {
}

void CalculationController::calculate(const int32_t localTimerCounter,
                                      const int32_t localTicValue,
                                      const unsigned long lastOverflow,
                                      const OpMode mode) {
    timeKeeping(lastOverflow);

    if (state_.isFirstTic) {
        state_.isFirstTic = false;
        updateSnapshots(localTimerCounter);
        return;
    }

    timerCounterNormalization(localTimerCounter, lastOverflow);
    ticLinearization(localTicValue);
    ticPreFilter();
    computeFrequencyError();
    piLoop(mode);
    lockDetection(mode);

#ifdef DEBUG_CALCULATION
    Serial2.print(F("Time: "));
    Serial2.print(state_.time);
    Serial2.print(F(", Timer Counter Real: "));
    Serial2.print(state_.timerCounterValueReal);
    Serial2.print(F(", Timer Counter Error: "));
    Serial2.print(state_.timerCounterError);
    Serial2.print(F(", TIC Value: "));
    Serial2.print(state_.ticValue);
    Serial2.print(F(", TIC Correction: "));
    Serial2.print(state_.ticValueCorrection);
    Serial2.print(F(", TIC Correction Offset: "));
    Serial2.print(state_.ticValueCorrectionOffset);
    Serial2.print(F(", TIC Corrected Net Value: "));
    Serial2.print(state_.ticCorrectedNetValue);
    Serial2.print(F(", TIC Corrected Net Value Filtered: "));
    Serial2.print(state_.ticCorrectedNetValueFiltered);
    Serial2.print(F(", TIC Frequency Error: "));
    Serial2.print(state_.ticFrequencyError);
    Serial2.print(F(", I Accumulator: "));
    Serial2.print(state_.iAccumulator, 4);
    Serial2.print(F(", P term: "));
    Serial2.print(state_.pTerm, 4);
    Serial2.print(F(", PPS Lock count: "));
    Serial2.print(state_.ppsLockCount);
    Serial2.print(F(", DAC Voltage: "));
    Serial2.print(state_.dacVoltage, 4);
    Serial2.print(F(", DAC Value: "));
    Serial2.print(state_.dacValue);
    Serial2.print(F(", Mode: "));
    Serial2.println(mode);

#endif

    updateSnapshots(localTimerCounter);
}

void CalculationController::timeKeeping(const unsigned long lastOverflow) {
    state_.time = static_cast<int32_t>(state_.time + (lastOverflow + 50) / 100);

    if (state_.time - state_.timeOld > 1) {
        state_.missedPpsCounter++;
        state_.timeSinceMissedPps = 0;
    }
    else {
        state_.timeSinceMissedPps++;
    }
}

void CalculationController::timerCounterNormalization(const int32_t localTimerCounter,
                                                      const unsigned long lastOverflow) {
    int32_t timerCounterValueReal = localTimerCounter - state_.timerCounterValueOld;
    // wrap around detection
    if (timerCounterValueReal < -(MODULO / 2)) {
        timerCounterValueReal += MODULO;
    }
    state_.timerCounterValueReal = timerCounterValueReal;
    state_.timerCounterValueOld = localTimerCounter;
    state_.timerCounterError = static_cast<int32_t>(COUNTS_PER_PPS - timerCounterValueReal - lastOverflow * MODULO);
}

void CalculationController::ticLinearization(const int32_t localTicValue) {
    // Polynomial linearization of tic value using a cubic correction polynomial.
    // Coefficients x2 and x3 are user-tunable; x1 is derived so that the polynomial
    // has unity gain at full scale (x1 = 1 - x2*1000 - x3*1e6, using pre-scaled coefficients).
    //
    // Horner form:  linearize(tic) = s * (x1 + s * (x2 + s * x3))
    // where s = (tic - ticMin) / (ticMax - ticMin) * 1000   (normalised to 0-1000)
    //
    // ticCorrectedNetValue = linearize(ticValue) - linearize(ticOffset)
    // This centres the output on zero at ticOffset, ready for the PID loop.

    state_.ticValue = localTicValue;

    // Recover the "logical" x1 from the pre-scaled coefficients for the unity-gain constraint.
    // Pre-scaled:  x2_stored = x2/1000, x3_stored = x3/100000
    // Unity-gain:  x1 + x2 + x3 = 1  =>  x1 = 1 - x2_stored*1000 - x3_stored*100000
    const double x1 = 1.0
        - state_.x2Coefficient * 1000.0
        - state_.x3Coefficient * 100000.0;

    auto linearize = [&](const double tic) -> double {
        const double normalizedTic = (tic - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0;
        return normalizedTic * (x1 + normalizedTic * (state_.x2Coefficient + normalizedTic * state_.x3Coefficient));
    };

    state_.ticValueCorrectionOffset = linearize(state_.ticOffset);
    // ReSharper disable once CppRedundantCastExpression
    state_.ticValueCorrection = linearize(static_cast<double>(state_.ticValue));
    state_.ticCorrectedNetValue = state_.ticValueCorrection - state_.ticValueCorrectionOffset;
    // the expectation is that ticCorrectedNetValue is now centred on zero at ticOffset, so the PI loop can treat it as a signed error value.
}

void CalculationController::ticPreFilter() {
    if (!state_.ticFilterSeeded) {
        state_.ticCorrectedNetValueFiltered = state_.ticCorrectedNetValue;
        state_.ticFilterSeeded = true;
        return;
    }

    // EMA: filtered += (raw - filtered) / filterConst
    // A natural lock detector follows: once abs(filtered) stays below a
    // threshold for N * filterConst consecutive seconds, the loop is locked.
    state_.ticCorrectedNetValueFiltered +=
        (state_.ticCorrectedNetValue - state_.ticCorrectedNetValueFiltered) / static_cast<double>(state_.
            ticFilterConst);
}

void CalculationController::computeFrequencyError() {
    if (state_.ticFilterSeeded) {
        // Rate of change of the phase error (counts/second ≈ ns/s ≈ ppb).
        // Uses the offset-subtracted value so the zero point is consistent with
        // ticCorrectedNetValue and ticCorrectedNetValueFiltered.
        state_.ticFrequencyError = state_.ticCorrectedNetValue - (state_.ticValueCorrectionOld - state_.
            ticValueCorrectionOffset);
    }
}

void CalculationController::piLoop(const OpMode mode) {
    // Only run the control loop in RUN mode.
    if (mode != RUN) {
        return;
    }

    // --- P-term ---
    // Proportional to the frequency error (rate of phase change, ns/s ≈ ppb).
    // This provides fast damping: when the OCXO frequency is drifting, the P-term
    // pushes the DAC in the right direction immediately.
    // The raw sawtooth produces frequency errors of ±400–600 counts/s, which at
    // gain=12 would be ±5000–7000 DAC counts — far too large. Clamp to
    // ±PTERM_MAX_COUNTS so the I-term remains in control of the long-term value.
    double pTerm = state_.ticFrequencyError * state_.gain;
    if (pTerm > PTERM_MAX_COUNTS) pTerm = PTERM_MAX_COUNTS;
    if (pTerm < -PTERM_MAX_COUNTS) pTerm = -PTERM_MAX_COUNTS;
    state_.pTerm = pTerm;

    // --- I-term (one step) ---
    // Integrates the filtered phase error toward zero over time.
    // Dividing by damping * timeConst makes the integrator slow and stable.
    // The remainder from the previous tick is added back to avoid truncation drift.
    const double iStep = (state_.ticCorrectedNetValueFiltered * state_.gain
            / static_cast<double>(state_.damping)
            / static_cast<double>(state_.timeConst))
        + state_.iRemainder;

    const double iStepFloor = floor(iStep);
    state_.iRemainder = iStep - iStepFloor; // carry the fractional part forward
    state_.iAccumulator += iStepFloor;

    // --- Clamp accumulator to prevent integrator wind-up ---
    // The accumulator represents the long-term DAC value, so it must stay
    // within the DAC range.
    if (state_.iAccumulator < static_cast<double>(state_.dacMinValue)) {
        state_.iAccumulator = static_cast<double>(state_.dacMinValue);
        state_.iRemainder = 0.0;
    }
    if (state_.iAccumulator > static_cast<double>(state_.dacMaxValue)) {
        state_.iAccumulator = static_cast<double>(state_.dacMaxValue);
        state_.iRemainder = 0.0;
    }

    // Record drift for lock detection before we forget the previous value.
    state_.iAccumulatorLast = state_.iAccumulator - iStepFloor;

    // --- Combine: integrator bias + proportional correction ---
    const double dacOutput = state_.iAccumulator + pTerm;

    // --- Clamp to DAC limits and write ---
    const auto dacClamped = static_cast<uint16_t>(
        dacOutput < static_cast<double>(state_.dacMinValue)
            ? static_cast<double>(state_.dacMinValue)
            : dacOutput > static_cast<double>(state_.dacMaxValue)
            ? static_cast<double>(state_.dacMaxValue)
            : dacOutput);

    state_.dacValue = dacClamped;
    state_.dacVoltage = static_cast<float>(dacClamped) / static_cast<float>(DAC_MAX_VALUE) * DAC_VREF;
    setDac_(dacClamped);
}

void CalculationController::lockDetection(const OpMode mode) {
    // Lock is only meaningful while the PI loop is running.
    if (mode != RUN) {
        state_.ppsLocked = false;
        state_.ppsLockCount = 0;
        return;
    }

    const double absFiltered = abs(state_.ticCorrectedNetValueFiltered);

    if (state_.ppsLocked) {
        // Already locked — unlock immediately if the filtered error exceeds the unlock threshold.
        if (absFiltered > UNLOCK_THRESHOLD) {
            state_.ppsLocked = false;
            state_.ppsLockCount = 0;

#ifdef DEBUG_CALCULATION
            Serial2.println(F("LOCK LOST"));
#endif
        }
    }
    else {
        // Not yet locked — both conditions must hold to count toward lock:
        //   1. Filtered phase error within ±LOCK_THRESHOLD (loop is near zero phase error).
        //   2. Integrator drift < LOCK_INTEGRATOR_DRIFT_MAX counts/tick (loop has converged —
        //      the integrator is no longer pulling in).
        // Either excursion resets the counter.
        const double iDrift = abs(state_.iAccumulator - state_.iAccumulatorLast);
        const bool phaseOk = absFiltered < LOCK_THRESHOLD;
        const bool integratorOk = iDrift < LOCK_INTEGRATOR_DRIFT_MAX;

        if (phaseOk && integratorOk) {
            state_.ppsLockCount++;

            // Require 2 × ticFilterConst consecutive seconds before declaring lock.
            // This ensures the EMA has fully settled before we call it locked.
            if (state_.ppsLockCount >= state_.ticFilterConst * 2) {
                state_.ppsLocked = true;

#ifdef DEBUG_CALCULATION
                Serial2.println(F("LOCKED"));
#endif
            }
        }
        else {
            // Any excursion outside either threshold resets the counter.
            state_.ppsLockCount = 0;
        }
    }
}

void CalculationController::updateSnapshots(const int32_t localTimerCounter) {
    state_.timeOld = state_.time;
    state_.timerCounterValueOld = localTimerCounter;
    state_.ticValueOld = state_.ticValue;
    state_.ticValueCorrectionOld = state_.ticValueCorrection;
}
