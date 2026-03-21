/**
@brief TimeZoneInfo
With the use of the tzdata your project can now run in local time.
Getting UTC time is easy, (GPS, NTP, ...) but local time is not.
GMT+x works mostly only for half of a year. (Daylight-Saving-Time) DST is evil!
You can't calculate it! This is why linux uses a database to make DST work.
So, why not using it for your Arduino?
It is a bit "heavy", prox. 4kB for the database of one location + the code to make it work.

https://github.com/rstephan/TimeZoneInfo

@warning This is a 32-Bit version! Year 2038 will come fast!

@author Stephan Ruloff
@date 24.04.2016
@copyright GPLv2
*/

#include "TimeZoneInfo.h"


TimeZoneInfo::TimeZoneInfo() = default;

void TimeZoneInfo::setLocation_P(const byte* tzFile) {
    mTzFile  = const_cast<byte*>(tzFile);
    mCharPos = 0;
    mCharLen = 0;
    memset(&mTimeInfo, 0, sizeof(mTimeInfo));
}

int32_t TimeZoneInfo::utc2local(const int32_t utc) {
    const int32_t offset = findTimeInfo(utc);

    return utc + offset;
}

int32_t TimeZoneInfo::local2utc(const int32_t local) {
    const int32_t offs = findTimeInfo(local);

    return local - offs;
}

// e.g. CEST/CET
String TimeZoneInfo::getShortName() {
    uint32_t pos = mTimeInfo.ttAbbrInd;
    String s;

    if (mCharPos) {
        while (pos < mCharLen) {
            uint8_t c = read8(mCharPos + pos);
            if (c == 0) {
                break;
            }
            s += static_cast<char>(c);
            pos++;
        }
    }

    return s;
}

boolean TimeZoneInfo::isDst() const {
    return mTimeInfo.ttIsDst != 0;
}

uint32_t TimeZoneInfo::read32(unsigned long pos) {
    uint32_t val = 0;

    val += static_cast<uint32_t>(pgm_read_byte(&mTzFile[pos + 0])) << 24;
    val += static_cast<uint32_t>(pgm_read_byte(&mTzFile[pos + 1])) << 16;
    val += static_cast<uint32_t>(pgm_read_byte(&mTzFile[pos + 2])) << 8;
    val += static_cast<uint32_t>(pgm_read_byte(&mTzFile[pos + 3]));

    return val;
}

uint8_t TimeZoneInfo::read8(unsigned long pos) {
    return pgm_read_byte(&mTzFile[pos]);
}

int32_t TimeZoneInfo::findTimeInfo(int32_t t) {
    boolean found = false;

    const uint32_t magic = read32(0);
    if (magic != 0x545a6966) {
        return 0;
    }
    const uint32_t leapCount = read32(28);
    const uint32_t timeCount = read32(32);
    const uint32_t typeCount = read32(36);
    const uint32_t charCount = read32(40);

    uint32_t i           = 0;
    uint32_t valTimeOld  = read32(44);
    for (i = 1; i < timeCount; i++) {
        const uint32_t valTime = read32(44 + (i * 4));
        if (static_cast<uint32_t>(t) >= valTimeOld && static_cast<uint32_t>(t) < valTime) {
            found = true;
            i--;
            break;
        }
        valTimeOld = valTime;
    }
    if (found) {
        const uint32_t valIndex = read8(44L + (timeCount * 4) + i);
        if (valIndex < typeCount) {
            const unsigned long pos = 44L + (timeCount * 4) + timeCount + (sizeof(ttInfo) * valIndex);
            mTimeInfo.ttGmtOffset     = static_cast<int32_t>(read32(pos));
            mTimeInfo.ttIsDst      = static_cast<int8_t>(read8(pos + 4));
            mTimeInfo.ttAbbrInd    = read8(pos + 5);
            mCharPos                = 44L + (timeCount * 5) + (sizeof(ttInfo) * typeCount) + (8 * leapCount);
            mCharLen                = charCount;
        }
    }

    return mTimeInfo.ttGmtOffset;
}
