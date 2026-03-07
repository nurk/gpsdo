#ifndef EEPROMCONTROLLER_H
#define EEPROMCONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Arduino.h>
#include <../src/CalculationController.h>
#include <../src/Constants.h>

class EEPROMController {
public:
    EEPROMController() = default;
    void begin();
    bool loadState(CalculationController& calculationController) const;
    void saveState(const CalculationController& calculationController);
    void invalidate();

private:
    // Persisted layout (packed, fixed-width)
    struct __attribute__((packed)) PersistedState {
        uint32_t magic; // 4 - magic value to indicate plausibility
        // ControlState-derived
        float iTerm; // 4
        int32_t iTermLong; // 4
        float iTermRemain; // 4
        float pTerm; // 4
        int32_t dacValue; // 4
        uint16_t dacValueOut; // 2 - actual 16-bit DAC output value (0..65535)
        uint16_t holdValue; // 2

        uint16_t timeConst; // 2
        uint8_t filterDiv; // 1
        uint16_t filterConst; // 2
        uint16_t ticOffset; // 2

        float tempCoefficient; // 4
        float tempReference; // 4
        float gain; // 4
        float damping; // 4

        int32_t ticValueFiltered; // 4 - helpful internal filter state

        // Small long-term highlights
        uint32_t restarts; // 4
        uint32_t totalTime3h; // 4
        uint32_t ticAverage3h; // 4
        uint32_t tempAverage3h; // 4
        uint32_t dacAverage3h; // 4
        uint16_t k; // 2
    };

    // change this magic number when PersistedState layout changes
    // to avoid loading incompatible data
    static constexpr uint32_t kMagic = 0x47505344UL;
    static constexpr int kEepromBaseAddr = 0; // start of storage
    static constexpr int kEepromMaxSize = 100; // 100 bytes reserved for state.

    // compile-time safety: ensure persisted struct fits in expected EEPROM
    static_assert(sizeof(PersistedState) <= kEepromMaxSize, "PersistedState exceeds 100 bytes");

    bool isValid_ = false;
};

#endif // EEPROMCONTROLLER_H
