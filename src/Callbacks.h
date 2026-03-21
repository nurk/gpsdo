#ifndef CALLBACKS_H
#define CALLBACKS_H

#include <Arduino.h>
#include <Constants.h>

void setDacValue(uint16_t value);

float readTemperatureC();

float readOCXOTemperatureC();

void saveState(const EEPROMState& eepromState);

void manuallySaveState();

void setOpMode(OpMode mode, int32_t holdValue);

#endif // CALLBACKS_H
