#ifndef EXTERNAL_EEPROM_CONTROLLER_H
#define EXTERNAL_EEPROM_CONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Arduino.h>
#include <I2C_eeprom.h>
#include <Constants.h>
#include <CalculationController.h>

class ExternalEEPROMController {
public:
    explicit ExternalEEPROMController(I2C_eeprom& eeprom) : eeprom_(eeprom) {}
    void begin();
    bool loadState(CalculationController& calculationController) const;
    void saveState(const CalculationController& calculationController);
    void invalidate();

private:
    static constexpr uint32_t kMagic = 0x47505350UL;
    static constexpr uint16_t kBankSize = 2048; // bytes per bank
    static constexpr uint8_t kBankCount = 8; // total banks (2048*8 = 16384 bytes)
    static constexpr uint16_t kHeaderSize = 8; // 4 bytes magic + 4 bytes seq
    static constexpr uint32_t kEepromBaseAddr = 0;

    static constexpr size_t kPayloadSize = sizeof(ControlState) + sizeof(LongTermControlState);
    static_assert(kPayloadSize + kHeaderSize <= kBankSize, "Payload does not fit in bank");

    uint32_t bankAddr(uint8_t bank) const noexcept;
    uint32_t readLE32(const uint8_t* p) const noexcept;
    void writeLE32(uint8_t* p, uint32_t v) const noexcept;

    I2C_eeprom& eeprom_;
    bool isValid_ = false;
    uint8_t activeBank_ = 0xFF;
    uint32_t activeSeq_ = 0;
};

#endif // EXTERNAL_EEPROM_CONTROLLER_H
