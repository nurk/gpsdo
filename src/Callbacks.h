#ifndef CALLBACKS_H
#define CALLBACKS_H

void setWarmupTime(uint16_t seconds);

void setDacValue(uint16_t value);

float readTemperatureC();

float readOCXOTemperatureC();

void saveState();

void manuallySaveState();

void setTCA0Count(uint16_t count);

void setOpMode(OperationMode mode, int32_t holdValue);

#endif // CALLBACKS_H
