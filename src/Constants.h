#ifndef GPSDO_V1_0_CONSTANTS_H
#define GPSDO_V1_0_CONSTANTS_H

// ReSharper disable once CppUnusedIncludeDirective
#include <Arduino.h>

enum OperationMode {
    run,
    hold
};

constexpr uint16_t DAC_MAX_VALUE = 65535;
constexpr uint16_t WARMUP_TIME_DEFAULT = 300; // seconds

using SetWarmupTimeFn = void(*)(uint16_t seconds);
using SetDacFn = void(*)(uint16_t value);
using ReadTempFn = float(*)();
using ReadOCXOTempFn = float(*)();
using SaveStateFn = void(*)();
using ManuallySaveStateFn = void(*)();
using SetTCA0CountFn = void(*)(uint16_t count);
using SetOpModeFn = void(*)(OperationMode mode, int32_t holdValue);

struct GpsData {
    bool isPositionValid = false;
    double latitude = 0.0;
    double longitude = 0.0;

    bool isSatellitesValid = false;
    uint32_t satellites = 0;

    bool isDateValid = false;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;

    bool isTimeValid = false;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t centisecond = 0;
};

struct ControlState {
    /* PI-loop state */
    float iTerm = 0.0f;
    float pTerm = 0.0f;
    int32_t iTermLong = 0;
    float iTermRemain = 0.0f;

    float gain = 12.0f;
    float damping = 3.0f;

    /* PPS / time bookkeeping */
    uint16_t missedPps = 0;
    uint32_t timeSinceMissedPps = 0;

    /* DAC values (internal scaled units) */
    int32_t dacValue = 0; // internal integrator accumulator (may exceed 16 bits)
    int32_t dacValueOut = 0; // scaled output before division by timeConst
    uint16_t dacOut = 0;
    uint16_t holdValue = 0;

    /* PPS lock detection */
    int16_t lockPpsLimit = 100;
    int16_t lockPpsFactor = 5;
    uint32_t lockPpsCounter = 0;
    bool ppsLocked = false;

    /* Time/filtering constants */
    uint16_t timeConst = 32;
    uint16_t timeConstOld = 32;
    uint8_t filterDiv = 2;
    uint16_t filterConst = 16;
    uint16_t filterConstOld = 16;

    /* Timer and error tracking */
    uint32_t time = 0;
    uint32_t timeOld = 0;
    uint16_t timerCounterValueOld = 0;
    int32_t timerUs = 0;
    int32_t timerUsOld = 0;
    int32_t diffNs = 0;
    int32_t diffNsForPpsLock = 0;

    /* TIC (phase/ADC) variables */
    uint16_t ticValue = 0;
    uint16_t ticValueOld = 0;
    uint16_t ticOffset = 500;
    int32_t ticValueFiltered = 0;
    int32_t ticValueFilteredOld = 0;
    int32_t ticValueFilteredForPpsLock = 0;

    /* TIC linearization */
    float ticMin = 12.0f;
    float ticMax = 1012.0f;
    float x3 = 0.03f;
    float x2 = 0.1f;
    float x1 = 0.0f;
    float ticValueCorrection = 0.0f;
    float ticValueCorrectionOld = 0.0f;
    float ticValueCorrectionOffset = 0.0f;

    /* Temperature compensation */
    // todo changed from 30 to 23
    float tempReferenceC = 24.0f;
    float tempC = 0.0f;
    float tempFilteredC = 24.0f;
    float tempCoefficientC = 0.0f;
};

struct LongTermControlState {
    int32_t i = 0; // counter for 300secs before storing temp and dac readings average
    int32_t j = 0; // counter for stored 300sec readings
    int32_t k = 0; // counter for stored 3hour readings

    //300sec storage
    uint32_t ticArray[144] = {};
    uint32_t tempArray[144] = {};
    uint32_t dacArray[144] = {};

    int32_t sumTic = 0;
    int32_t sumTic2 = 0;
    int32_t sumTemp = 0;
    int32_t sumTemp2 = 0;
    uint32_t sumDac = 0;
    uint32_t sumDac2 = 0;

    uint32_t ticAverage3h = 0;
    uint32_t tempAverage3h = 0;
    uint32_t dacAverage3h = 0;

    uint32_t totalTime3h = 0; // counter for power-up time updated every third hour
    uint32_t restarts = 0; // counter for restarts/power-ups
    bool restartFlag = true;
};
#endif //GPSDO_V1_0_CONSTANTS_H
