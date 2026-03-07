#include <../src/EEPROMController.h>
#include <EEPROM.h>

void EEPROMController::begin() {
    // Read magic (first 4 bytes) and set validity flag
    uint32_t magic = 0;
    magic = EEPROM.read(kEepromBaseAddr) | (EEPROM.read(kEepromBaseAddr + 1) << 8) |
        (EEPROM.read(kEepromBaseAddr + 2) << 16) | (EEPROM.read(kEepromBaseAddr + 3) << 24);
    isValid_ = (magic == kMagic);
}

bool EEPROMController::loadState(CalculationController& calculationController) const {
    if (!isValid_) return false;

    PersistedState p{};
    constexpr int size = sizeof(PersistedState);

    auto* dst = reinterpret_cast<uint8_t*>(&p);
    for (int i = 0; i < size; ++i) {
        dst[i] = EEPROM.read(kEepromBaseAddr + i);
    }

    // Additional sanity: check magic again
    if (p.magic != kMagic) return false;

    // Map into controller (control state)
    calculationController.state().iTerm = p.iTerm;
    calculationController.state().iTermLong = p.iTermLong;
    calculationController.state().iTermRemain = p.iTermRemain;
    calculationController.state().pTerm = p.pTerm;
    calculationController.state().dacValue = p.dacValue; // internal accumulator
    calculationController.state().dacValueOut = static_cast<int32_t>(p.dacValueOut);

    calculationController.state().holdValue = p.holdValue;

    calculationController.state().timeConst = p.timeConst;
    calculationController.state().filterDiv = p.filterDiv;
    calculationController.state().timeConstOld = p.timeConst; // keep consistency
    calculationController.state().filterConst = p.filterConst; // approximate

    calculationController.state().ticOffset = p.ticOffset;

    calculationController.state().tempCoefficientC = p.tempCoefficient;
    calculationController.state().tempReferenceC = p.tempReference;
    calculationController.state().gain = p.gain;
    calculationController.state().damping = p.damping;

    calculationController.state().ticValueFiltered = p.ticValueFiltered;

    // long-term
    calculationController.longTermState().restarts = p.restarts;
    calculationController.longTermState().totalTime3h = p.totalTime3h;
    calculationController.longTermState().ticAverage3h = p.ticAverage3h;
    calculationController.longTermState().tempAverage3h = p.tempAverage3h;
    calculationController.longTermState().dacAverage3h = p.dacAverage3h;
    calculationController.longTermState().k = p.k;

    return true;
}

void EEPROMController::saveState(const CalculationController& calculationController) {
    PersistedState p{};
    // write payload with magic cleared first to avoid marking a partially-written
    // image as valid. We'll write the magic bytes after the payload.
    p.magic = 0;

    p.iTerm = calculationController.state().iTerm;
    p.iTermLong = calculationController.state().iTermLong;
    p.iTermRemain = calculationController.state().iTermRemain;
    p.pTerm = calculationController.state().pTerm;
    p.dacValue = calculationController.state().dacValue;
    p.dacValueOut = static_cast<uint16_t>(calculationController.state().dacValueOut);
    p.holdValue = calculationController.state().holdValue;

    p.timeConst = calculationController.state().timeConst;
    p.filterDiv = calculationController.state().filterDiv;
    p.filterConst = calculationController.state().filterConst;
    p.ticOffset = calculationController.state().ticOffset;

    p.tempCoefficient = calculationController.state().tempCoefficientC;
    p.tempReference = calculationController.state().tempReferenceC;
    p.gain = calculationController.state().gain;
    p.damping = calculationController.state().damping;

    p.ticValueFiltered = calculationController.state().ticValueFiltered;

    p.restarts = calculationController.longTermState().restarts;
    p.totalTime3h = calculationController.longTermState().totalTime3h;
    p.ticAverage3h = calculationController.longTermState().ticAverage3h;
    p.tempAverage3h = calculationController.longTermState().tempAverage3h;
    p.dacAverage3h = calculationController.longTermState().dacAverage3h;

    p.k = static_cast<uint16_t>(calculationController.longTermState().k);

    constexpr int size = sizeof(PersistedState);

    const auto* src = reinterpret_cast<const uint8_t*>(&p);
    // write payload bytes (magic is zero here)
    for (int i = 0; i < size; ++i) {
        EEPROM.update(kEepromBaseAddr + i, src[i]);
    }

    // Now write magic last (little-endian)
    EEPROM.update(kEepromBaseAddr + 0, kMagic & 0xFF);
    EEPROM.update(kEepromBaseAddr + 1, kMagic >> 8 & 0xFF);
    EEPROM.update(kEepromBaseAddr + 2, kMagic >> 16 & 0xFF);
    EEPROM.update(kEepromBaseAddr + 3, kMagic >> 24 & 0xFF);

    // After writing, set isValid_
    isValid_ = true;
}

void EEPROMController::invalidate() {
    // clear magic and mark invalid (write zero magic)
    EEPROM.update(kEepromBaseAddr + 0, 0);
    EEPROM.update(kEepromBaseAddr + 1, 0);
    EEPROM.update(kEepromBaseAddr + 2, 0);
    EEPROM.update(kEepromBaseAddr + 3, 0);
    isValid_ = false;
}
