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
    // TIME KEEPING
    state_.time = static_cast<int32_t>(state_.time + (lastOverflow + 50) / 100);

    if (state_.time - state_.timeOld > 1) {
        state_.missedPpsCounter++;
        state_.timeSinceMissedPps = 0;
    }
    else {
        state_.timeSinceMissedPps++;
    }

    // TIMER COUNTER
    int32_t timerCounterValueReal = localTimerCounter - state_.timerCounterValueOld;
    // wrap around detection
    if (timerCounterValueReal < -(MODULO / 2)) {
        timerCounterValueReal += MODULO;
    }
    state_.timerCounterValueReal = timerCounterValueReal;
    state_.timerCounterValueOld = localTimerCounter;
    state_.timerCounterError = static_cast<int32_t>(COUNTS_PER_PPS - timerCounterValueReal - lastOverflow * MODULO);

    // TIC LINEARIZATION
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

    auto linearize = [&](const int32_t tic) -> double {
        const double s = (static_cast<double>(tic) - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0;
        return s * (x1 + s * (state_.x2Coefficient + s * state_.x3Coefficient));
    };
    auto linearizeD = [&](const double tic) -> double {
        const double s = (tic - TIC_MIN) / (TIC_MAX - TIC_MIN) * 1000.0;
        return s * (x1 + s * (state_.x2Coefficient + s * state_.x3Coefficient));
    };

    state_.ticValueCorrectionOffset = linearizeD(state_.ticOffset);
    state_.ticValueCorrection       = linearize(state_.ticValue);
    state_.ticCorrectedNetValue = state_.ticValueCorrection - state_.ticValueCorrectionOffset;
    // the expectation is that ticCorrectedNetValue is now centred on zero at ticOffset, so the PI loop can treat it as a signed error value.

#ifdef DEBUG_CALCULATION


#endif
}

void CalculationController::updateSnapshots(const int32_t localTimerCounter) {
    state_.timeOld = state_.time;
    state_.timerCounterValueOld = localTimerCounter;
    state_.ticValueOld = state_.ticValue;
    state_.ticValueCorrectionOld = state_.ticValueCorrection;
}
