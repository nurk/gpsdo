#ifndef CALCULATION_CONTROLLER_H
#define CALCULATION_CONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Arduino.h>
#include <Constants.h>

class CalculationController {
public:
    CalculationController(SetDacFn setDac,
                          ReadTempFn readTemp,
                          SaveStateFn saveState,
                          SetTCA0CountFn setTCA0Count);

    void calculate(unsigned int localTimerCounter,
                   int localTicValue,
                   unsigned long lastOverflow,
                   bool isRun,
                   uint16_t warmupTime);

    const ControlState& state() const { return state_; }
    ControlState& state() { return state_; }

    const LongTermControlState& longTermState() const { return longTermState_; }
    LongTermControlState& longTermState() { return longTermState_; }

    void resetShortTermAccumulators();

    void setTemperatureCoefficient(const float coefficient) {
        state_.tempCoefficientC = coefficient;
    }

    void setTemperatureReference(const float tempC) {
        state_.tempReferenceC = tempC;
    }

    void setTimeConstant(const uint16_t timeConst) {
        state_.timeConst = timeConst;
    }

    void setTicMin(const float ticMin) {
        state_.ticMin = ticMin;
    }

    void setTicMax(const float ticMax) {
        state_.ticMax = ticMax;
    }

    void setX2(const float x2) {
        state_.x2 = x2;
    }

private:
    ControlState state_;
    LongTermControlState longTermState_;

    SetDacFn setDac_;
    ReadTempFn readTemp_;
    SaveStateFn saveState_;
    SetTCA0CountFn setTCA0Count_;

    long computeTempCorrectionScaled() const;
    void updateLongTermState(bool isRun);

    // Helpers extracted from calculate() to improve readability and testability.
    void preloadHeuristic(uint16_t warmupTime) const;
    void ticLinearization(int localTicValue);
    void updateTimersAndResets(unsigned int localTimerCounter,
                               unsigned long lastOverflow,
                               bool isRun,
                               uint16_t warmupTime);
    void ppsLockDetection(uint16_t warmupTime);
    void determineFilterConstAndRescale(bool isRun);
    void lowPassTicFilter();
    void piLoop(bool isRun, uint16_t warmupTime);
    void tempCompensationAndDacOutput(bool isRun);
    void updateSnapshots(unsigned int localTimerCounter);
};

#endif // CALCULATION_CONTROLLER_H
