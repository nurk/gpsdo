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

    void calculate(int32_t localTimerCounter,
                   int32_t localTicValue,
                   unsigned long lastOverflow,
                   OpMode mode);

    const ControlState& state() const { return state_; }
    ControlState& state() { return state_; }

private:
    ControlState state_;

    SetDacFn setDac_;
    ReadTempFn readTemp_;
    SaveStateFn saveState_;
    SetTCA0CountFn setTCA0Count_;

    void timeKeeping(unsigned long lastOverflow);
    void timerCounterNormalization(int32_t localTimerCounter, unsigned long lastOverflow);
    void ticLinearization(int32_t localTicValue);
    void ticPreFilter();
    void computeFrequencyError();
    void piLoop(OpMode mode);
    void lockDetection(OpMode mode);
    void updateSnapshots(int32_t localTimerCounter);
};

#endif // CALCULATION_CONTROLLER_H
