// ReSharper disable CppMemberFunctionMayBeStatic
#include <ExternalEEPROMController.h>

// file-scoped reusable buffer sized to the payload to avoid dynamic allocation on small MCUs
static uint8_t g_payloadBuf[sizeof(ControlState) + sizeof(LongTermControlState)];

// Helper implementations now as member functions
uint32_t ExternalEEPROMController::bankAddr(const uint8_t bank) const noexcept {
    return kEepromBaseAddr + static_cast<uint32_t>(bank) * kBankSize;
}

uint32_t ExternalEEPROMController::readLE32(const uint8_t* p) const noexcept {
    return static_cast<uint32_t>(p[0]) |
        static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 |
        static_cast<uint32_t>(p[3]) << 24;
}

void ExternalEEPROMController::writeLE32(uint8_t* p, const uint32_t v) const noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8 & 0xFF);
    p[2] = static_cast<uint8_t>(v >> 16 & 0xFF);
    p[3] = static_cast<uint8_t>(v >> 24 & 0xFF);
}

void ExternalEEPROMController::begin() {
    // scan banks to find the highest valid seq
    uint32_t bestSeq = 0u;
    int bestBank = -1;
    uint8_t header[kHeaderSize];

    for (uint8_t b = 0; b < kBankCount; ++b) {
        const uint32_t addr = bankAddr(b);
        eeprom_.readBlock(addr, header, kHeaderSize);
        const uint32_t magic = readLE32(header);
        if (magic != kMagic) continue;
        const uint32_t seq = readLE32(header + 4);
        if (seq >= bestSeq) {
            bestSeq = seq;
            bestBank = static_cast<int>(b);
        }
    }

    if (bestBank >= 0) {
        isValid_ = true;
        activeBank_ = static_cast<uint8_t>(bestBank);
        activeSeq_ = bestSeq;
    }
    else {
        isValid_ = false;
        activeBank_ = 0xFFu;
        activeSeq_ = 0u;
    }
}

bool ExternalEEPROMController::loadState(CalculationController& calculationController) const {
    if (!isValid_) return false;

    const uint32_t addr = bankAddr(activeBank_) + kHeaderSize;
    constexpr size_t payloadNeeded = sizeof(ControlState) + sizeof(LongTermControlState);

    // read directly into static buffer
    eeprom_.readBlock(addr, g_payloadBuf, payloadNeeded);

    // reconstruct and apply
    // Avoid large stack allocations by copying payload directly into the controller's
    // state objects. Creating a local LongTermControlState on the stack can overflow
    // SRAM on small MCUs and cause a crash when loading persisted data.
    memcpy(&calculationController.state(), g_payloadBuf, sizeof(ControlState));
    memcpy(&calculationController.longTermState(), g_payloadBuf + sizeof(ControlState),
           sizeof(LongTermControlState));

    return true;
}

void ExternalEEPROMController::saveState(const CalculationController& calculationController) {
#ifdef DEBUG
    Serial2.println(F("Saving controller state to EEPROM started"));
#endif
    constexpr size_t payloadNeeded = sizeof(ControlState) + sizeof(LongTermControlState);

    // prepare payload into static buffer
    memcpy(g_payloadBuf, &calculationController.state(), sizeof(ControlState));
    memcpy(g_payloadBuf + sizeof(ControlState), &calculationController.longTermState(),
           sizeof(LongTermControlState));

    // choose next bank and seq
    const uint8_t nextBank = isValid_ ? static_cast<uint8_t>((activeBank_ + 1) % kBankCount) : 0u;
    const uint32_t nextSeq = isValid_ ? (activeSeq_ + 1u) : 1u;
    const uint32_t destAddr = bankAddr(nextBank);

    // write payload first (after header)
    eeprom_.updateBlock(destAddr + kHeaderSize, g_payloadBuf, payloadNeeded);

    // write seq and magic last
    uint8_t header[kHeaderSize];
    writeLE32(header, kMagic);
    writeLE32(header + 4, nextSeq);
    eeprom_.updateBlock(destAddr, header, kHeaderSize);

    // update runtime state
    isValid_ = true;
    activeBank_ = nextBank;
    activeSeq_ = nextSeq;
#ifdef DEBUG
    Serial2.println(F("Saving controller state to EEPROM ended"));
#endif
}

void ExternalEEPROMController::invalidate() {
    // clear both magic and seq on all banks
    constexpr uint8_t zeros[kHeaderSize] = {};
    for (uint8_t b = 0; b < kBankCount; ++b) {
        const uint32_t addr = bankAddr(b);
        eeprom_.updateBlock(addr, zeros, kHeaderSize);
    }
    isValid_ = false;
    activeBank_ = 0xFFu;
    activeSeq_ = 0u;
}
