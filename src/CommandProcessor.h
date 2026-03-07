#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

// ReSharper disable once CppUnusedIncludeDirective
#include <Stream.h>
#include <CalculationController.h>
#include <SerialOutputController.h>
#include <Constants.h>
#include <ExternalEEPROMController.h>

class EEPROMController;

class CommandProcessor {
public:
    CommandProcessor(Stream& serialPort,
                     SetDacFn setDac,
                     SetWarmupTimeFn setWarmupTime,
                     SetOpModeFn setOpMode,
                     ManuallySaveStateFn manuallySaveState,
                     CalculationController& calculationController,
                     SerialOutputController& serialOutputController,
                     ExternalEEPROMController& eepromController);

    void process() const;

private:
    Stream& serial_;
    SetDacFn setDac_;
    SetWarmupTimeFn setWarmupTime_;
    SetOpModeFn setOpMode_;
    ManuallySaveStateFn manuallySaveState_;
    CalculationController& controller_;
    SerialOutputController& serialOutputController_;
    ExternalEEPROMController& eepromController_; // changed to reference for uniformity

    void printHelp() const;

    void setWarmupTime(uint16_t value) const;

    void setDac(uint16_t value) const;
};

#endif // COMMAND_PROCESSOR_H
