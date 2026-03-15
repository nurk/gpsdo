// ReSharper disable CppMemberFunctionMayBeStatic
#include <ExternalEEPROMController.h>

// Stored payload layout (little-endian, no padding assumed):
//   bytes 0–1  : dacValue    (uint16_t)
//   bytes 2–9  : iAccumulator (double, 8 bytes)
// Total: 10 bytes. Must match kPayloadSize in the header.
static constexpr uint16_t kDacValueOffset       = 0;
static constexpr uint16_t kIAccumulatorOffset   = kDacValueOffset + sizeof(uint16_t);
static constexpr uint16_t kStoredPayloadSize    = kIAccumulatorOffset + sizeof(double);

// Verify the stored size matches what the header advertises.
// kPayloadSize in the header is defined as sizeof(uint16_t) + sizeof(double),
// so this assert is a compile-time cross-check between the two translation units.
static_assert(kStoredPayloadSize == sizeof(uint16_t) + sizeof(double),
              "kStoredPayloadSize does not match kPayloadSize definition in header");

static uint8_t g_payloadBuf[kStoredPayloadSize];

uint32_t ExternalEEPROMController::bankAddr(const uint8_t bank) const noexcept { // NOLINT(*-convert-member-functions-to-static)
    return kEepromBaseAddr + static_cast<uint32_t>(bank) * kBankSize;
}

uint32_t ExternalEEPROMController::readLE32(const uint8_t* p) const noexcept { // NOLINT(*-convert-member-functions-to-static)
    return static_cast<uint32_t>(p[0]) |
        static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 |
        static_cast<uint32_t>(p[3]) << 24;
}

void ExternalEEPROMController::writeLE32(uint8_t* p, const uint32_t v) const noexcept { // NOLINT(*-convert-member-functions-to-static)
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
        if (seq > bestSeq) {
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

EEPROMState ExternalEEPROMController::loadState() const {
    EEPROMState eepromState; // isValid defaults to false
    if (!isValid_) return eepromState;

    const uint32_t addr = bankAddr(activeBank_) + kHeaderSize;
    eeprom_.readBlock(addr, g_payloadBuf, kStoredPayloadSize);

    // Deserialise fields explicitly — never read isValid from EEPROM.
    memcpy(&eepromState.dacValue,     g_payloadBuf + kDacValueOffset,     sizeof(uint16_t));
    memcpy(&eepromState.iAccumulator, g_payloadBuf + kIAccumulatorOffset, sizeof(double));
    eepromState.isValid = true; // set only after a successful read

    return eepromState;
}

void ExternalEEPROMController::saveState(const EEPROMState& eepromState) {
#ifdef DEBUG
    Serial2.println(F("Saving controller state to EEPROM started"));
#endif

    // Serialise fields explicitly — never write isValid to EEPROM.
    memcpy(g_payloadBuf + kDacValueOffset,     &eepromState.dacValue,     sizeof(uint16_t));
    memcpy(g_payloadBuf + kIAccumulatorOffset, &eepromState.iAccumulator, sizeof(double));

    // choose next bank and seq
    const uint8_t  nextBank = isValid_ ? static_cast<uint8_t>((activeBank_ + 1) % kBankCount) : 0u;
    const uint32_t nextSeq  = isValid_ ? (activeSeq_ + 1u) : 1u;
    const uint32_t destAddr = bankAddr(nextBank);

    // write payload first (after header) — if power is lost here the old header is still valid
    eeprom_.updateBlock(destAddr + kHeaderSize, g_payloadBuf, kStoredPayloadSize);

    // write magic and seq last — this atomically commits the bank
    uint8_t header[kHeaderSize];
    writeLE32(header,     kMagic);
    writeLE32(header + 4, nextSeq);
    eeprom_.updateBlock(destAddr, header, kHeaderSize);

    // update runtime state
    isValid_    = true;
    activeBank_ = nextBank;
    activeSeq_  = nextSeq;
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
