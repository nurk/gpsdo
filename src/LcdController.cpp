#include <LcdController.h>

#include <CalculationController.h>
#include <math.h> // NOLINT(*-deprecated-headers)

LcdController::LcdController(hd44780_I2Cexp& lcd,
                             CalculationController& calculationController,
                             ReadTempFn readTempFn,
                             ReadOCXOTempFn readOCXOTempFn)
    : lcd_(lcd),
      calculationController_(calculationController),
      readTempFn(readTempFn),
      readOCXOTempFn(readOCXOTempFn) {
    opMode_ = run;
}

void LcdController::update(const int page) {
    if (millis() > lastUpdateMillis_ + 215 || page != currentPage_) {
        currentPage_ = page;

        switch (currentPage_) {
            case 0:
                drawPageZero();
                break;
            case 1:
                drawPageOne();
                break;
            case 2:
                drawPageTwo();
                break;
            case 3:
                drawPageThree();
                break;
            default:
                drawPageZero();
                break;
        }

        lastUpdateMillis_ = millis();
    }
}

// Private helper moved out of drawPageZero
void LcdController::formatDMS(const double value, char* out, const bool isLatitude) {
    const double absVal = fabs(value);
    const int deg = static_cast<int>(floor(absVal));
    const double remMin = (absVal - deg) * 60.0;
    const int minutes = static_cast<int>(floor(remMin));
    const double seconds = (remMin - minutes) * 60.0;

    // Use strictly ASCII labels to avoid LCD character set mismatches.
    // Inline hemisphere selection to avoid an intermediate variable
    if (isLatitude) {
        // use sizeof fixed buffer (callers pass a 21-byte buffer)
        snprintf(out, 21, "%03d\xDF %02d'%04.1f\" %c",
                 deg, minutes, seconds, (value < 0.0) ? 'S' : 'N');
    }
    else {
        snprintf(out, 21, "%03d\xDF %02d'%04.1f\" %c",
                 deg, minutes, seconds, (value < 0.0) ? 'W' : 'E');
    }
}

void LcdController::drawPageZero() const {
    const float temp = readTempFn();
    const float ocxoTemp = readOCXOTempFn();

    lcd_.clear();

    // Row 0: Latitude in DMS (or placeholder)
    char latBuf[21];
    if (gpsData_.isPositionValid) {
        formatDMS(gpsData_.latitude, latBuf, true);
    }
    else {
        snprintf(latBuf, sizeof(latBuf), "---\xDF --'--.-\"  ");
    }
    lcd_.setCursor(0, 0);
    lcd_.print(F("Lat: "));
    lcd_.print(latBuf);

    // Row 1: Longitude in DMS (or placeholder)
    char lonBuf[21];
    if (gpsData_.isPositionValid) {
        formatDMS(gpsData_.longitude, lonBuf, false);
    }
    else {
        snprintf(lonBuf, sizeof(lonBuf), "---\xDF --'--.-\"  ");
    }
    lcd_.setCursor(0, 1);
    lcd_.print(F("Lon: "));
    lcd_.print(lonBuf);

    // Row 2: Date and Time on one line: DD/MM/YYYY HH:MM:SS
    // Show date or placeholder separately from time
    lcd_.setCursor(0, 2);
    char datePart[12];
    char timePart[10];
    if (gpsData_.isDateValid) {
        snprintf(datePart, sizeof(datePart), "%02u/%02u/%04u",
                 static_cast<unsigned>(gpsData_.day),
                 static_cast<unsigned>(gpsData_.month),
                 static_cast<unsigned>(gpsData_.year));
    }
    else {
        snprintf(datePart, sizeof(datePart), "--/--/----");
    }
    if (gpsData_.isTimeValid) {
        snprintf(timePart, sizeof(timePart), "%02u:%02u:%02u",
                 static_cast<unsigned>(gpsData_.hour),
                 static_cast<unsigned>(gpsData_.minute),
                 static_cast<unsigned>(gpsData_.second));
    }
    else {
        snprintf(timePart, sizeof(timePart), "--:--:--");
    }
    char dateTimeBuf[21];
    snprintf(dateTimeBuf, sizeof(dateTimeBuf), "%s %s", datePart, timePart);
    lcd_.print(dateTimeBuf);

    // Row 3: Temperatures (one decimal) and satellite count; show S:- if satellites invalid
    lcd_.setCursor(0, 3);
    char tempSatBuf[21];
    if (gpsData_.isSatellitesValid) {
        snprintf(tempSatBuf, sizeof(tempSatBuf), "T1:%04.1f T2:%04.1f S:%u",
                 temp,
                 ocxoTemp,
                 static_cast<unsigned>(gpsData_.satellites));
    }
    else {
        snprintf(tempSatBuf, sizeof(tempSatBuf), "T1:%04.1f T2:%04.1f S:-",
                 static_cast<double>(temp),
                 static_cast<double>(ocxoTemp));
    }
    lcd_.print(tempSatBuf);
}

void LcdController::drawPageOne() const {
    lcd_.clear();

    const auto& s = calculationController_.state();
    const auto& l = calculationController_.longTermState();

    char line[21]; // 20 chars + NUL

    // Row 0: PPS lock status and missed PPS counter
    lcd_.setCursor(0, 0);
    const char* lockStr = s.ppsLocked ? "LOCKED" : "UNLOCKED";
    snprintf(line, sizeof(line), "PPS:%s Miss:%3u",
             lockStr,
             static_cast<unsigned>(s.missedPps));
    lcd_.print(line);

    // Row 1: Current timing error (diffNs) and filter constants
    lcd_.setCursor(0, 1);
    snprintf(line, sizeof(line), "Err:%+06ld F:%03u T:%03u",
             static_cast<long>(s.diffNs),
             static_cast<unsigned>(s.filterConst),
             static_cast<unsigned>(s.timeConst));
    lcd_.print(line);

    // Row 2: Short-term jitter (filtered delta) and proportional term (pTerm)
    lcd_.setCursor(0, 2);
    long jitter = static_cast<long>(s.ticValueFiltered) - static_cast<long>(s.ticValueFilteredOld);
    if (jitter < 0) jitter = -jitter;
    snprintf(line, sizeof(line), "Jit:%+05ld p:%04.1f",
             jitter,
             static_cast<double>(s.pTerm));
    lcd_.print(line);

    // Row 3: 3-hour TIC average and filtered temperature (use placeholders if no long-term data yet)
    lcd_.setCursor(0, 3);
    if (l.totalTime3h == 0) {
        // not enough long-term data yet
        snprintf(line, sizeof(line), "3hT:----- tmp:--.-");
    }
    else {
        snprintf(line, sizeof(line), "3hT:%05lu tmp:%04.1f",
                 static_cast<unsigned long>(l.ticAverage3h),
                 static_cast<double>(s.tempFilteredC));
    }
    lcd_.print(line);
}

void LcdController::drawPageTwo() const {
    lcd_.clear();
    const auto& s = calculationController_.state();

    char line[21];

    // Row 0: DAC current output (visible 16-bit DAC value)
    lcd_.setCursor(0, 0);
    // Show the actual 16-bit DAC output that is written to hardware (dacOut),
    // not the internal scaled accumulator (dacValueOut).
    snprintf(line, sizeof(line), "Dac: %05u",
             static_cast<unsigned>(s.dacOut));
    lcd_.print(line);

    // Row 1: DAC hold/target value and hold flag
    lcd_.setCursor(0, 1);
    // Use unsigned formatting for holdValue (uint16_t)
    snprintf(line, sizeof(line), "Hold: %05u",
             static_cast<unsigned>(s.holdValue));
    lcd_.print(line);

    // Row 2: Dac voltage
    lcd_.setCursor(0, 2);
    const double dacVoltage = (static_cast<double>(s.dacOut) / static_cast<double>(DAC_MAX_VALUE)) * 5.0;
    snprintf(line, sizeof(line), "Dac Volts: %6.4fV",
             dacVoltage);
    // Print the formatted voltage line to the LCD (was missing, causing the line not to appear)
    lcd_.print(line);

    // Row 3: Hold indicator
    lcd_.setCursor(0, 3);
    if (opMode_ == hold) {
        lcd_.print("HOLD mode");
    }
}

void LcdController::drawPageThree() const {
    lcd_.clear();
    const auto& l = calculationController_.longTermState();

    char line[21];

    // Row 0: Long-term sigma computed from sumTic / sumTic2 if samples available
    lcd_.setCursor(0, 0);
    // We'll use l.j as the number of 5-minute samples stored (guarded >1)
    const long n = l.j;
    if (n > 1) {
        // Compute variance = (sum2 - (sum*sum)/n)/(n-1)
        const auto sum = static_cast<double>(l.sumTic);
        const auto sum2 = static_cast<double>(l.sumTic2);
        const double var = (sum2 - (sum * sum) / static_cast<double>(n)) / static_cast<double>(n - 1);
        const double sigma = (var > 0.0) ? sqrt(var) : 0.0;
        // sigma is in the same units as tic (raw units); print with one decimal
        snprintf(line, sizeof(line), "sigma:%06.1f n:%03ld",
                 static_cast<double>(sigma), n);
    }
    else {
        snprintf(line, sizeof(line), "sigma:----- n:---");
    }
    lcd_.print(line);

    // Row 1: 3-hour TIC average (again) and restarts
    lcd_.setCursor(0, 1);
    snprintf(line, sizeof(line), "3h:%05lu R:%03lu",
             static_cast<unsigned long>(l.ticAverage3h),
             static_cast<unsigned long>(l.restarts));
    lcd_.print(line);
}
