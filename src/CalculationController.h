#ifndef CALCULATION_CONTROLLER_H
#define CALCULATION_CONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Arduino.h>
#include <Constants.h>

class CalculationController {
public:
    CalculationController(SetDacFn setDac,
                          ReadTempFn readTemp,
                          ReadOCXOTempFn readOCXOTemp,
                          SaveStateFn saveState);

    void calculate(int32_t localTimerCounter,
                   int32_t localTicValue,
                   unsigned long lastOverflow,
                   OpMode opMode);

    const ControlState& state() const { return state_; }
    ControlState& state() { return state_; }

    EEPROMState getEEPROMState() const;
    void setEEPROMState(const EEPROMState& eepromState);

private:
    ControlState state_;

    SetDacFn setDac_;
    ReadTempFn readTemp_;
    ReadOCXOTempFn readOCXOTemp_;
    SaveStateFn saveState_;

    void timeKeeping(unsigned long lastOverflow);
    void timerCounterNormalization(int32_t localTimerCounter, unsigned long lastOverflow);
    void ticLinearization(int32_t localTicValue);
    void ticPreFilter();
    void computeFrequencyError();
    void piLoop(OpMode mode);
    void lockDetection(OpMode mode);
    void updateSnapshots(int32_t localTimerCounter);
    void storeState(OpMode opMode);
};

#endif // CALCULATION_CONTROLLER_H
