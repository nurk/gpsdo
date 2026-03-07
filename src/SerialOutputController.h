#ifndef SERIALOUTPUTCONTROLLER_H
#define SERIALOUTPUTCONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Stream.h>
#include <CalculationController.h>
#include <Arduino.h>
#include <Constants.h>

class SerialOutputController {
public:
    SerialOutputController(Stream& out, CalculationController& calculationController);

    // Print a compact on-demand summary of current control state
    void printSummary() const;

    // Print detailed ControlState fields
    void printControlState() const;

    // Print detailed LongTermControlState fields (3-hour aggregates and indexes)
    void printLongTermState() const;

    // Print all entries of the 5-minute arrays (tic, temp, dac)
    void printLongTermAll() const;

private:
    Stream& out_;
    CalculationController& controller_;

    void printKeyValue(const __FlashStringHelper* key, uint32_t value) const;
    void printKeyValue(const __FlashStringHelper* key, int32_t value) const;
    void printKeyValue(const __FlashStringHelper* key, uint16_t value) const;
    void printKeyValue(const __FlashStringHelper* key, float value) const;
    void printKeyValue(const __FlashStringHelper* key, bool value) const;
};

#endif // SERIALOUTPUTCONTROLLER_H
