#include <SerialOutputController.h>

SerialOutputController::SerialOutputController(Stream& out, CalculationController& calculationController)
    : out_(out), controller_(calculationController) {
}

void SerialOutputController::printKeyValue(const __FlashStringHelper* key, const uint32_t value) const {
    out_.print(key);
    out_.print(F(": "));
    out_.println(static_cast<unsigned long>(value));
}

void SerialOutputController::printKeyValue(const __FlashStringHelper* key, const int32_t value) const {
    out_.print(key);
    out_.print(F(": "));
    out_.println(static_cast<long>(value));
}

void SerialOutputController::printKeyValue(const __FlashStringHelper* key, const uint16_t value) const {
    out_.print(key);
    out_.print(F(": "));
    out_.println(static_cast<unsigned int>(value));
}

void SerialOutputController::printKeyValue(const __FlashStringHelper* key, const float value) const {
    out_.print(key);
    out_.print(F(": "));
    out_.println(value);
}

void SerialOutputController::printKeyValue(const __FlashStringHelper* key, const bool value) const {
    out_.print(key);
    out_.print(F(": "));
    out_.println(value ? 1 : 0);
}

void SerialOutputController::printSummary() const {
    const auto& s = controller_.state();
    const auto& l = controller_.longTermState();

    out_.println(F("--- Controller Summary ---"));
    printKeyValue(F("time (s)"), s.time);
    printKeyValue(F("ppsLocked"), s.ppsLocked);
    printKeyValue(F("ticValue"), s.ticValue);
    printKeyValue(F("ticFiltered"), s.ticValueFiltered);
    printKeyValue(F("dacValueOut"), s.dacValueOut);
    printKeyValue(F("timeConst"), s.timeConst);
    printKeyValue(F("filterConst"), s.filterConst);
    printKeyValue(F("longTerm.j"), l.j);
    printKeyValue(F("longTerm.k"), l.k);
    printKeyValue(F("3h tic avg"), l.ticAverage3h);
    printKeyValue(F("3h temp avg"), l.tempAverage3h);
    printKeyValue(F("3h dac avg"), l.dacAverage3h);
    out_.println();
}

void SerialOutputController::printControlState() const {
    const auto& s = controller_.state();
    out_.println(F("--- ControlState ---"));
    printKeyValue(F("time (s)"), s.time);
    printKeyValue(F("timeOld"), s.timeOld);
    printKeyValue(F("timerUs"), s.timerUs);
    printKeyValue(F("timerUsOld"), s.timerUsOld);
    printKeyValue(F("diffNs"), s.diffNs);
    printKeyValue(F("timerCounterValueOld"), s.timerCounterValueOld);
    printKeyValue(F("ticValue"), s.ticValue);
    printKeyValue(F("ticValueOld"), s.ticValueOld);
    printKeyValue(F("ticOffset"), s.ticOffset);
    printKeyValue(F("ticValueFiltered"), s.ticValueFiltered);
    printKeyValue(F("pTerm"), s.pTerm);
    printKeyValue(F("iTerm"), s.iTerm);
    printKeyValue(F("dacValue"), s.dacValue);
    printKeyValue(F("dacValueOut"), s.dacValueOut);
    printKeyValue(F("tempC"), s.tempC);
    printKeyValue(F("tempFilteredC"), s.tempFilteredC);
    printKeyValue(F("tempCoefficientC"), s.tempCoefficientC);
    printKeyValue(F("tempReferenceC"), s.tempReferenceC);
    out_.println();
}

void SerialOutputController::printLongTermState() const {
    const auto& l = controller_.longTermState();
    out_.println(F("--- LongTermControlState ---"));
    printKeyValue(F("i (samples)"), l.i);
    printKeyValue(F("j (5min idx)"), l.j);
    printKeyValue(F("k (3h idx)"), l.k);
    printKeyValue(F("totalTime3h"), l.totalTime3h);
    printKeyValue(F("restarts"), l.restarts);
    printKeyValue(F("ticAverage3h"), l.ticAverage3h);
    printKeyValue(F("tempAverage3h"), l.tempAverage3h);
    printKeyValue(F("dacAverage3h"), l.dacAverage3h);
    out_.println();
}

void SerialOutputController::printLongTermAll() const {
    const auto& l = controller_.longTermState();
    out_.println(F("--- Long-term all entries (0..143) ---"));
    printKeyValue(F("Current index"), l.k);
    for (unsigned int idx = 0; idx < 144; ++idx) {
        out_.print(F("idx "));
        out_.print(idx);
        out_.print(F(": tic="));
        out_.print(l.ticArray[idx]);
        out_.print(F(" temp="));
        out_.print(l.tempArray[idx]);
        out_.print(F(" dac="));
        out_.println(l.dacArray[idx]);
    }
    out_.println();
}
