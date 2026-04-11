#include <CalculationController.h>
#include <Arduino.h>

CalculationController::CalculationController(const SetDacFn setDac,
                                             const ReadTempFn readTemp,
                                             const ReadOCXOTempFn readOCXOTemp,
                                             const SaveStateFn saveState)
    : setDac_(setDac),
      readTemp_(readTemp),
      readOCXOTemp_(readOCXOTemp),
      saveState_(saveState) {
}

void CalculationController::calculate(const int32_t localTimerCounter,
                                      const int32_t localTicValue,
                                      const unsigned long lastOverflow,
                                      const OpMode opMode) {
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
    piLoop(opMode);
    lockDetection(opMode);
    storeState(opMode);

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
    Serial2.print(F(", Coarse Trim: "));
    Serial2.print(state_.lastCoarseTrim, 4);
    Serial2.print(F(", Coarse Error Accumulator: "));
    Serial2.print(state_.coarseErrorAccumulator, 4);
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
    Serial2.println(opMode);
#endif

    updateSnapshots(localTimerCounter);
}

void CalculationController::timeKeeping(const unsigned long lastOverflow) {
    state_.time = static_cast<int32_t>(state_.time + (lastOverflow + 50) / 100);

    if (state_.time - state_.timeOld > 1) {
        state_.missedPpsCounter++;
        state_.timeSinceMissedPps = 0;
    } else {
        state_.timeSinceMissedPps++;
    }
}

void CalculationController::timerCounterNormalization(const int32_t localTimerCounter,
                                                      const unsigned long lastOverflow) {
    int32_t timerCounterValueReal = localTimerCounter - state_.timerCounterValueOld;
    // Wrap-around detection — handle both directions.
    // TCA0 counts 0 … MODULO-1.  The raw difference can be anywhere in
    // [-(MODULO-1) … +(MODULO-1)].  We expect a value very close to 0
    // (a few counts of OCXO jitter).  Any magnitude > MODULO/2 means the
    // counter wrapped; correct it back into the [-MODULO/2 … +MODULO/2] range.
    if (timerCounterValueReal < -(MODULO / 2)) {
        timerCounterValueReal += MODULO; // counter wrapped downward (captured value jumped back near 0)
    } else if (timerCounterValueReal > (MODULO / 2)) {
        timerCounterValueReal -= MODULO; // counter wrapped upward   (e.g. raw +49999 → corrected -1)
    }
    state_.timerCounterValueReal = timerCounterValueReal;
    state_.timerCounterValueOld  = localTimerCounter;

    // Compute timerCounterError using int32_t arithmetic to avoid unsigned long
    // overflow wrapping. lastOverflow * MODULO can exceed int32_t if lastOverflow
    // is a spurious large value (e.g. missed PPS ISR accumulating multiple seconds
    // of overflows). Clamp lastOverflow to the maximum plausible value first:
    // COUNTS_PER_PPS / MODULO = 100 overflows per second; allow up to 2× for safety.
    const auto clampedOverflow = static_cast<int32_t>(
        lastOverflow > 200UL ? 200UL : lastOverflow);
    state_.timerCounterError = COUNTS_PER_PPS - timerCounterValueReal - clampedOverflow * MODULO;
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
    state_.ticValueCorrection   = linearize(static_cast<double>(state_.ticValue));
    state_.ticCorrectedNetValue = state_.ticValueCorrection - state_.ticValueCorrectionOffset;
    // the expectation is that ticCorrectedNetValue is now centred on zero at ticOffset, so the PI loop can treat it as a signed error value.
}

void CalculationController::ticPreFilter() {
    if (!state_.ticFilterSeeded) {
        state_.ticCorrectedNetValueFiltered = state_.ticCorrectedNetValue;
        state_.ticFilterSeeded              = true;
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
    // Rate of change of the fine (TIC) phase error component.
    // Uses the offset-subtracted value so the zero point is consistent with
    // ticCorrectedNetValue and ticCorrectedNetValueFiltered.
    state_.ticDelta = state_.ticCorrectedNetValue - (state_.ticValueCorrectionOld - state_.ticValueCorrectionOffset);

    // timerCounterError is the coarse frequency error: how many 5 MHz counter ticks
    // (each = 200 ns) the OCXO gained or lost this second vs the GPS PPS.
    // Converting to the same units as ticDelta (linearised TIC counts ≈ ns):
    //   timerCounterError × 200 ns/count
    // This term captures frequency offsets that cause complete TIC wrap-arounds and
    // that ticDelta alone cannot see cleanly on every tick.
    state_.ticFrequencyError = state_.ticDelta + static_cast<double>(state_.timerCounterError) * 200.0;
}

void CalculationController::piLoop(const OpMode mode) {
    // Only run the control loop in RUN mode.
    if (mode != RUN) {
        return;
    }

    // --- P-term ---
    // Proportional to the rate of change of the fine TIC phase error (ticDelta).
    //
    // TIC wrap suppression: when the TIC sawtooth wraps (PPS edge crosses an OCXO
    // cycle boundary), ticDelta jumps by ~700–800 counts in the wrong direction —
    // a large negative spike when the OCXO is slow (wrap from high back to low) and
    // a large positive spike when fast (wrap from low back to high).  Both are the
    // opposite of what the P-term should do for the prevailing frequency error, so
    // they would kick the DAC the wrong way for one tick.
    // Detect wraps by |ticDelta| > TIC_WRAP_THRESHOLD and zero the P-term entirely
    // for that tick.  The I-term continues uninterrupted; the missing P-term for one
    // tick is inconsequential.
    //
    // On non-wrap ticks, clamp to ±PTERM_MAX_COUNTS as a safety net.
    double pTerm = 0.0;
    if (fabs(state_.ticDelta) <= TIC_WRAP_THRESHOLD) {
        pTerm = state_.ticDelta * state_.gain;
        if (pTerm > PTERM_MAX_COUNTS) pTerm = PTERM_MAX_COUNTS;
        if (pTerm < -PTERM_MAX_COUNTS) pTerm = -PTERM_MAX_COUNTS;
    }
    // pTerm stays 0.0 on wrap ticks.
    state_.pTerm = pTerm;

    // --- I-term (one step) ---
    // Integrates the filtered phase error toward zero over time.
    // Dividing by damping * timeConst makes the integrator slow and stable.
    // The remainder from the previous tick is added back to avoid truncation drift.
    //
    // I-term suppression: when the coarse trim has just fired a rail-recovery trim,
    // suppress the I-term for coarseTrimPeriod ticks so the coarse trim has a full
    // period to work uncontested before the I-term can drain the recovery back to the rail.
    if (state_.iTermSuppressCount > 0) {
        state_.iTermSuppressCount--;
        // Keep iRemainder zeroed during suppression so there is no accumulated
        // carry that would produce a large burst when suppression ends.
        state_.iRemainder = 0.0;
    } else {
        // --- Overshoot guard (conditional integration) ---
        // During a long pull-in the EMA filter builds up sustained history in one
        // direction.  After the OCXO crosses the true on-frequency EFC setpoint the
        // raw phase (ticCorrectedNetValue) changes sign every ~8 ticks as the TIC
        // sawtooth oscillates — that is normal behaviour, NOT an overshoot signal.
        // Using a single raw-vs-filtered sign comparison incorrectly freezes the
        // integrator on every ramp half-cycle (proven broken in run3 of 2026-04-10).
        //
        // The correct overshoot indicator uses TWO consecutive raw samples:
        //   If both the current AND the previous raw phase value are on the opposite
        //   side of zero from the filtered value, the loop has genuinely crossed and
        //   held the setpoint for at least 2 ticks — not just a momentary sawtooth
        //   crossing.  Freeze the integrator until raw and filtered agree again.
        //
        // A TIC sawtooth half-cycle at 200 ppb drift lasts ~4 ticks (ramp from 0 to
        // 488 counts at 100 counts/tick).  Two consecutive ticks on the wrong side
        // will occasionally catch a sawtooth crossing, but this is a harmless 1-tick
        // freeze compared to the many seconds of overshoot it prevents.
        //
        // During normal locked operation |filtered| < 50 counts and the raw signal
        // oscillates around zero with it, so this guard virtually never fires.
        const double prevNetValue   = state_.ticValueCorrectionOld - state_.ticValueCorrectionOffset;
        const bool filteredNeg      = state_.ticCorrectedNetValueFiltered < 0.0;
        const bool filteredPos      = state_.ticCorrectedNetValueFiltered > 0.0;
        const bool currRawOppFilter = (filteredNeg && state_.ticCorrectedNetValue > 0.0) ||
                                      (filteredPos && state_.ticCorrectedNetValue < 0.0);
        const bool prevRawOppFilter = (filteredNeg && prevNetValue > 0.0) ||
                                      (filteredPos && prevNetValue < 0.0);
        const bool overshootDetected = currRawOppFilter && prevRawOppFilter;

        const double iStep      = (state_.ticCorrectedNetValueFiltered * state_.gain
                / static_cast<double>(state_.damping)
                / static_cast<double>(state_.timeConst))
            + state_.iRemainder;
        const double iStepFloor = floor(iStep);

        if (overshootDetected) {
            // Both consecutive raw samples are opposite to the filter — genuine
            // setpoint crossing.  Freeze integrator; drain remainder.
            state_.iRemainder = 0.0;
        } else {
            // --- Anti-windup: do not apply an I-step that pushes deeper into a rail ---
            const bool atMin             = state_.iAccumulator <= static_cast<double>(state_.dacMinValue);
            const bool atMax             = state_.iAccumulator >= static_cast<double>(state_.dacMaxValue);
            const bool stepDrivesIntoMin = iStepFloor < 0.0;
            const bool stepDrivesIntoMax = iStepFloor > 0.0;

            if ((atMin && stepDrivesIntoMin) || (atMax && stepDrivesIntoMax)) {
                // Step would push further into the rail — discard it.
                state_.iRemainder = 0.0;
            } else {
                state_.iRemainder   = iStep - iStepFloor;
                state_.iAccumulator += iStepFloor;

                // --- Clamp accumulator to DAC range ---
                if (state_.iAccumulator < static_cast<double>(state_.dacMinValue)) {
                    state_.iAccumulator = static_cast<double>(state_.dacMinValue);
                    state_.iRemainder   = 0.0;
                }
                if (state_.iAccumulator > static_cast<double>(state_.dacMaxValue)) {
                    state_.iAccumulator = static_cast<double>(state_.dacMaxValue);
                    state_.iRemainder   = 0.0;
                }
            }
        }
    }

    // --- Coarse frequency trim ---
    // The fine TIC loop nulls the phase error (ticCorrectedNetValueFiltered → 0)
    // but cannot correct a residual frequency offset that causes the TIC sawtooth
    // to keep wrapping. timerCounterError captures this: each count = 200 ns of
    // frequency offset per second. A persistent non-zero mean (e.g. −0.78 in run9)
    // means the OCXO is consistently fast/slow by a sub-200 ns amount that the fine
    // loop has nulled in phase but not in frequency.
    //
    // This outer loop accumulates timerCounterError every tick and every
    // coarseTrimPeriod seconds applies a small trim to iAccumulator proportional
    // to the accumulated sum. The gain is deliberately tiny — this is a very slow
    // correction that must not fight the fine I-term.
    //
    // Anti-windup: same rail checks as the fine I-term apply.
    // Guard: only accumulate timerCounterError values within a plausible range.
    // A glitch tick (e.g. spurious overflow interrupt) can produce |error| >> 5;
    // accumulated over the period and multiplied by coarseTrimGain it would drive
    // iAccumulator to a rail 64 s later.  Discard anything beyond the sanity limit.
    // NOTE: use explicit comparison rather than abs() — the Arduino abs() macro
    // operates on int (16-bit on AVR) and is unsafe for int32_t values.
    if (state_.timerCounterError >= -COARSE_ERROR_SANITY_LIMIT &&
        state_.timerCounterError <= COARSE_ERROR_SANITY_LIMIT) {
        state_.coarseErrorAccumulator += static_cast<double>(state_.timerCounterError);
    }
    state_.lastCoarseTrim = 0.0;

    if (state_.time % state_.coarseTrimPeriod == 0) {
        const double coarseTrim       = state_.coarseErrorAccumulator * state_.coarseTrimGain;
        state_.coarseErrorAccumulator = 0.0;

        const bool coarseAtMin     = state_.iAccumulator <= static_cast<double>(state_.dacMinValue);
        const bool coarseAtMax     = state_.iAccumulator >= static_cast<double>(state_.dacMaxValue);
        const bool coarseDrivesMin = coarseTrim < 0.0;
        const bool coarseDrivesMax = coarseTrim > 0.0;
        const bool trimAway        = (coarseAtMin && coarseDrivesMax) || (coarseAtMax && coarseDrivesMin);

        if (!((coarseAtMin && coarseDrivesMin) || (coarseAtMax && coarseDrivesMax))) {
            state_.iAccumulator += coarseTrim;
            if (state_.iAccumulator < static_cast<double>(state_.dacMinValue))
                state_.iAccumulator = static_cast<double>(state_.dacMinValue);
            if (state_.iAccumulator > static_cast<double>(state_.dacMaxValue))
                state_.iAccumulator = static_cast<double>(state_.dacMaxValue);

            // If this trim pulled the accumulator off a rail, suppress the I-term
            // for one full coarseTrimPeriod so the trim has time to work before
            // the I-term can drain it back to the rail.
            if (trimAway) {
                state_.iTermSuppressCount = state_.coarseTrimPeriod;
                state_.iRemainder         = 0.0;
            }
        }

        state_.lastCoarseTrim = coarseTrim;
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

    state_.dacValue   = dacClamped;
    state_.dacVoltage = static_cast<float>(dacClamped) / static_cast<float>(DAC_MAX_VALUE) * DAC_VREF;
    setDac_(dacClamped);
}

void CalculationController::lockDetection(const OpMode mode) {
    // Lock is only meaningful while the PI loop is running.
    if (mode != RUN) {
        state_.ppsLocked    = false;
        state_.ppsLockCount = 0;
        return;
    }

    const double absFiltered = fabs(state_.ticCorrectedNetValueFiltered);

    if (state_.ppsLocked) {
        // Already locked — unlock immediately if the filtered error exceeds the unlock threshold.
        if (absFiltered > UNLOCK_THRESHOLD) {
            state_.ppsLocked    = false;
            state_.ppsLockCount = 0;
        }
    } else {
        // Not yet locked — the filtered phase error must stay within ±LOCK_THRESHOLD
        // for ticFilterConst consecutive seconds to declare lock.
        //
        // Rationale for ticFilterConst (not 2×):
        // With a persistent TIC sawtooth (period ~42 ticks, filterConst=16), the filtered
        // value stays below LOCK_THRESHOLD for only ~25 ticks per cycle (the trough).
        // Requiring 2×ticFilterConst (32 consecutive) is structurally impossible — the next
        // sawtooth peak always arrives within 31 ticks and resets the counter.
        // ticFilterConst (16) consecutive ticks is achievable in every trough once converged
        // and spans exactly one full EMA time constant — sufficient evidence of settlement.
        const bool phaseOk = absFiltered < LOCK_THRESHOLD;

        if (phaseOk) {
            state_.ppsLockCount++;

            if (state_.ppsLockCount >= state_.ticFilterConst) {
                state_.ppsLocked = true;
            }
        } else {
            // Any excursion outside either threshold resets the counter.
            state_.ppsLockCount = 0;
        }
    }
}

void CalculationController::updateSnapshots(const int32_t localTimerCounter) {
    state_.timeOld               = state_.time;
    state_.timerCounterValueOld  = localTimerCounter;
    state_.ticValueOld           = state_.ticValue;
    state_.ticValueCorrectionOld = state_.ticValueCorrection;
}

void CalculationController::storeState(const OpMode opMode) {
    // Three-phase save cadence based purely on elapsed time — no resetting counters.
    //
    //   Phase 1 — t < 3600 s  (first hour):        save every 10 minutes (600 s)
    //   Phase 2 — t < 43200 s (first 12 hours):    save every hour       (3600 s)
    //   Phase 3 — t >= 43200 s (after 12 hours):   save every 12 hours   (43200 s)
    //
    // state_.storeStateTime is seconds since RUN and is always increasing, so each phase
    // boundary is crossed exactly once and the modulo check gives the right cadence
    // within each phase with no counter resets needed.

    if (state_.time <= 0 || opMode != RUN) return;

    state_.storeStateTime++;

    constexpr int32_t kOneHour     = 3600;
    constexpr int32_t kTwelveHours = 43200;

    if (state_.storeStateTime < kOneHour) {
        // Phase 1: save every 10 minutes.
        constexpr int32_t kTenMinutes = 600;
        if (state_.storeStateTime % kTenMinutes == 0) {
            saveState_(getEEPROMState());
        }
    } else if (state_.storeStateTime < kTwelveHours) {
        // Phase 2: save every hour.
        if (state_.storeStateTime % kOneHour == 0) {
            saveState_(getEEPROMState());
        }
    } else {
        // Phase 3: save every 12 hours.
        if (state_.storeStateTime % kTwelveHours == 0) {
            saveState_(getEEPROMState());
        }
    }
}

EEPROMState CalculationController::getEEPROMState() const {
    return {
        state_.dacValue,
        state_.iAccumulator,
    };
}

void CalculationController::setEEPROMState(const EEPROMState& eepromState) {
    state_.dacValue     = eepromState.dacValue;
    state_.dacVoltage   = static_cast<float>(eepromState.dacValue) / static_cast<float>(DAC_MAX_VALUE) * DAC_VREF;
    state_.iAccumulator = eepromState.iAccumulator;
    state_.iRemainder   = 0.0; // always start fresh — remainder from previous session is meaningless
    setDac_(eepromState.dacValue); // drive the hardware immediately
}
