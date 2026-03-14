#include <CalculationController.h>
#include <Arduino.h>

CalculationController::CalculationController(const SetDacFn setDac,
                                             const ReadTempFn readTemp,
                                             const ReadOCXOTempFn readOCXOTemp,
                                             const SaveStateFn saveState,
                                             const SetTCA0CountFn setTCA0Count)
    : setDac_(setDac),
      readTemp_(readTemp),
      readOCXOTemp_(readOCXOTemp),
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
    Serial2.print(F(", TIC delta: "));
    Serial2.print(state_.ticDelta, 4);
    Serial2.print(F(", I Accumulator: "));
    Serial2.print(state_.iAccumulator, 4);
    Serial2.print(F(", I Remainder: "));
    Serial2.print(state_.iRemainder, 4);
    Serial2.print(F(", P term: "));
    Serial2.print(state_.pTerm, 4);
    Serial2.print(F(", PPS Lock count: "));
    Serial2.print(state_.ppsLockCount);
    Serial2.print(F(", DAC Voltage: "));
    Serial2.print(state_.dacVoltage, 4);
    Serial2.print(F(", DAC Value: "));
    Serial2.print(state_.dacValue);
    Serial2.print(F(", Temp OCXO: "));
    Serial2.print(readOCXOTemp_(), 2);
    Serial2.print(F(", Temp Board: "));
    Serial2.print(readTemp_(), 2);
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

    // EMA (Exponential Moving Average): filtered += (raw - filtered) / filterConst
    // A natural lock detector follows: once abs(filtered) stays below a
    // threshold for N * filterConst consecutive seconds, the loop is locked.
    state_.ticCorrectedNetValueFiltered +=
        (state_.ticCorrectedNetValue - state_.ticCorrectedNetValueFiltered) / static_cast<double>(state_.
            ticFilterConst);
}

void CalculationController::computeFrequencyError() {
    if (state_.ticFilterSeeded) {
        // Rate of change of the fine (TIC) phase error component.
        // Uses the offset-subtracted value so the zero point is consistent with
        // ticCorrectedNetValue and ticCorrectedNetValueFiltered.
        const double ticDelta = state_.ticCorrectedNetValue - (state_.ticValueCorrectionOld - state_.
            ticValueCorrectionOffset);

        state_.ticDelta = ticDelta;

        // timerCounterError is the coarse frequency error: how many 5 MHz counter ticks
        // (each = 200 ns) the OCXO gained or lost this second vs the GPS PPS.
        // Converting to the same units as ticDelta (linearised TIC counts ≈ ns):
        //   timerCounterError × 200 ns/count
        // This term captures frequency offsets that cause complete TIC wrap-arounds and
        // that ticDelta alone cannot see cleanly on every tick.
        const double coarseFreqError = static_cast<double>(state_.timerCounterError) * 200.0;

        state_.ticFrequencyError = ticDelta + coarseFreqError;
    }
}

void CalculationController::piLoop(const OpMode mode) {
    // Only run the control loop in RUN mode.
    if (mode != RUN) {
        return;
    }

    // --- P-term ---
    // Proportional to the rate of change of the fine TIC phase error (ticDelta),
    // NOT ticFrequencyError. ticFrequencyError includes the coarse counter term
    // (timerCounterError × 200 ns), which fires at ±200 ns on every GPS PPS jitter
    // tick of ±1 count. At gain=12 that produces ±2400 DAC counts — hitting the
    // clamp on almost every tick and masking the real signal.
    // ticDelta alone is the true rate of change of the linearised phase error and
    // is the correct signal to damp.
    // Clamp to ±PTERM_MAX_COUNTS so sawtooth wrap spikes don't overwhelm the I-term.
    double pTerm = state_.ticDelta * state_.gain;
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


    // --- Anti-windup: do not apply an I-step that pushes deeper into a rail ---
    // If the accumulator is already at the minimum clamp and the step would make
    // it more negative, discard the step (and its remainder) entirely.
    // Likewise for the maximum clamp. This prevents the integrator from winding
    // up indefinitely when the required EFC voltage is outside the DAC range.
    // (Electronic Frequency Control)
    const bool atMin = state_.iAccumulator <= static_cast<double>(state_.dacMinValue);
    const bool atMax = state_.iAccumulator >= static_cast<double>(state_.dacMaxValue);
    const bool stepDrivesIntoMin = iStepFloor < 0.0;
    const bool stepDrivesIntoMax = iStepFloor > 0.0;

    if ((atMin && stepDrivesIntoMin) || (atMax && stepDrivesIntoMax)) {
        // Step would push further into the rail — discard it.
        state_.iRemainder = 0.0;
    }
    else {
        state_.iRemainder = iStep - iStepFloor; // carry the fractional part forward
        state_.iAccumulator += iStepFloor;

        // --- Clamp accumulator to DAC range ---
        if (state_.iAccumulator < static_cast<double>(state_.dacMinValue)) {
            state_.iAccumulator = static_cast<double>(state_.dacMinValue);
            state_.iRemainder = 0.0;
        }
        if (state_.iAccumulator > static_cast<double>(state_.dacMaxValue)) {
            state_.iAccumulator = static_cast<double>(state_.dacMaxValue);
            state_.iRemainder = 0.0;
        }
    }

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
        // Not yet locked — the filtered phase error must stay within ±LOCK_THRESHOLD
        // for 2 × ticFilterConst consecutive seconds to declare lock.
        //
        // The original iDrift guard (|iAccumulator - iAccumulatorLast| < LOCK_INTEGRATOR_DRIFT_MAX)
        // was intended to block premature lock while the integrator is still pulling in.
        // However once the integrator has converged, the I-step still oscillates ±(filtered*0.125)
        // each tick, tracking the TIC sawtooth — so iDrift routinely exceeds the threshold even
        // when the mean drift is near zero. This permanently blocks lock declaration.
        // The EMA filter staying within ±LOCK_THRESHOLD for 2×ticFilterConst seconds already
        // provides sufficient confidence that the loop has settled. No separate drift guard needed.
        const bool phaseOk = absFiltered < LOCK_THRESHOLD;

        if (phaseOk) {
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
