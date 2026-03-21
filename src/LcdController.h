#ifndef GPSDO_V1_0_LCDCONTROLLER_H
#define GPSDO_V1_0_LCDCONTROLLER_H

// ReSharper disable CppUnusedIncludeDirective
#include <Arduino.h>
#include <Constants.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <CalculationController.h>

class LcdController {
public:
    LcdController(hd44780_I2Cexp& lcd,
                  CalculationController& calculationController,
                  ReadTempFn readTempFn,
                  ReadOCXOTempFn readOCXOTempFn);

    const GpsData& gpsData() const { return gpsData_; }
    GpsData& gpsData() { return gpsData_; }

    int pageCount() const { return pageCount_; }

    void update(int page, OpMode opMode);
    void giveActionFeedback(const String& actionFeedback);

private:
    enum LcdMode {
        INFO,
        ACTION
    };

    hd44780_I2Cexp& lcd_;
    GpsData gpsData_;
    CalculationController& calculationController_;

    ReadTempFn readTempFn;
    ReadOCXOTempFn readOCXOTempFn;

    void drawPageZero() const;
    void drawPageOne(OpMode opMode) const;
    void drawPageTwo() const;
    void drawPageThree() const;
    void drawActionFeedbackPage() const;

    // Formats decimal degrees into a DMS string (DDD° MM' SS.S")
    // If isLatitude==true, appends N or S; otherwise appends E or W.
    static void formatDMS(double value, char* out, bool isLatitude);

    int currentPage_                   = 0;
    int pageCount_                     = 4;
    unsigned long lastUpdateMillis_    = 0;
    LcdMode lcdMode_                   = INFO;
    unsigned long actionModeEndMillis_ = 0;
    String actionFeedback_             = "";
};

#endif //GPSDO_V1_0_LCDCONTROLLER_H
