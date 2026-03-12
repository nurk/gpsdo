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
    char line[21];

    lcd_.setCursor(0, 0);
    snprintf(line, sizeof(line), "Dac Volts: %6.4fV",
             calculationController_.state().dacVoltage);
    lcd_.print(line);

    lcd_.setCursor(0, 1);
    lcd_.print(opMode_);
}
