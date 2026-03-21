#ifndef TIMEZONEINFO_H
#define TIMEZONEINFO_H

#include <Arduino.h>

// https://github.com/rstephan/TimeZoneInfo

struct ttInfo {
    int32_t ttGmtOffset;
    int8_t ttIsDst;
    uint8_t ttAbbrInd;
};

class TimeZoneInfo {
public:
    TimeZoneInfo();
    void setLocation_P(const byte* tzFile);
    int32_t utc2local(int32_t utc);
    int32_t local2utc(int32_t local);
    String getShortName(); // e.g. CEST/CET
    boolean isDst() const;

private:
    int32_t findTimeInfo(int32_t t);
    uint32_t read32(unsigned long pos);
    uint8_t read8(unsigned long pos);
    byte* mTzFile          = nullptr;
    ttInfo mTimeInfo       = {0, 0, 0};
    unsigned long mCharPos = 0;
    uint32_t mCharLen      = 0;
};

#endif
