/**
 * Software revision: 1.0
 * Hardware revision: 1.0
 *
 * Libs:
 *
 * Ublox: https://github.com/sparkfun/SparkFun_u-blox_GNSS_v3
 * Ublox: https://github.com/mikalhart/TinyGPSPlus
 * JCButton: https://github.com/JChristensen/JC_Button
 * LM75B: https://github.com/jeremycole/Temperature_LM75_Derived
 * DAC: https://github.com/RobTillaart/DAC8571
 * Rotary Encoder: https://github.com/mathertel/RotaryEncoder
 * LCD: https://github.com/duinoWitchery/hd44780
 *
 *
 * Ublox connected to Serial1 and I2C
 * Debug header connected to Serial2
 **/

#include <Callbacks.h>
#include <Arduino.h>
#include <DAC8571.h>
#include <JC_Button.h>
#include <RotaryEncoder.h>
#include <TinyGPSPlus.h>
#include <Temperature_LM75_Derived.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <util/atomic.h>
#include <CalculationController.h>
#include <Constants.h>
#include <LcdController.h>
#include <avr/wdt.h>
#include <I2C_eeprom.h>
#include <ExternalEEPROMController.h>

#define ROTARY_PUSH PIN_PD3
#define ROTARY_A PIN_PA0
#define ROTARY_B PIN_PA1
#define BUTTON_A PIN_PC2
#define BUTTON_B PIN_PC3
#define LOCK_LED PIN_PD4
#define HEATING_LED PIN_PD5
#define PPS_ERROR_LED PIN_PD6
#define RESET_GPS PIN_PD7

#define TIC PIN_PD0
#define PPS_IN PIN_PA4
#define OCXO_IN PIN_PA5

// 128Kbit EEPROM
I2C_eeprom eeprom(0x50, I2C_DEVICESIZE_24LC128); // NOLINT(*-interfaces-global-init)

Button buttonA(BUTTON_A);
Button buttonB(BUTTON_B);
Button rotaryButton(ROTARY_PUSH);

RotaryEncoder encoder(ROTARY_A, ROTARY_B, RotaryEncoder::LatchMode::FOUR3);
int encoderPosition = 0;

hd44780_I2Cexp lcd(0x27);
int lcdPage = 0;

Generic_LM75_11Bit temperature(0x48);
Generic_LM75_11Bit temperatureOCXO(0x49);

DAC8571 dac(0x4C); // NOLINT(*-interfaces-global-init)

// GPS watchdog: track last byte received and last reset time
unsigned long lastGpsReceiveMillis                  = 0;
unsigned long lastGpsResetMillis                    = 0;
static constexpr unsigned long GPS_RESET_TIMEOUT_MS = 600000UL; // 600 seconds
TinyGPSPlus gps;

uint16_t warmupTime = WARMUP_TIME_DEFAULT; // seconds
unsigned long warmupEndMillis;

CalculationController calculationController(setDacValue,
                                            readTemperatureC,
                                            readOCXOTemperatureC,
                                            saveState,
                                            setTCA0Count);
LcdController lcdController(lcd,
                            calculationController,
                            readTemperatureC,
                            readOCXOTemperatureC);
ExternalEEPROMController externalEepromController(eeprom);

OpMode opMode = WARMUP;

// ISR variables
volatile int32_t timerCounterValue;
volatile int32_t ticValue;
volatile bool ppsReady                   = false;
volatile bool adcReady                   = false;
volatile unsigned long overflowCount     = 0;
volatile unsigned long lastOverflowCount = 0;

bool ppsError = true;

bool manualSaveRequested           = false;
unsigned long lastManualSaveMillis = 0;

void saveState(const EEPROMState& eepromState) {
#ifdef DEBUG_SAVE
    Serial2.print(F("Saving controller state to EEPROM:"));
    Serial2.print(F("  DAC Value: "));
    Serial2.print(eepromState.dacValue);
    Serial2.print(F("  I Accumulator: "));
    Serial2.println(eepromState.iAccumulator, 4);
#endif

    externalEepromController.saveState(eepromState);
}

void manuallySaveState() {
    manualSaveRequested = true;
}

void doCalculation() {
    // Snapshot ISR-shared variables atomically
    int32_t localTimerCounter       = 0;
    int32_t localTicValue           = 0;
    unsigned long localLastOverflow = 0;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        localTimerCounter = timerCounterValue;
        localLastOverflow = lastOverflowCount;
        localTicValue     = ticValue;
    }

    calculationController.calculate(localTimerCounter, localTicValue, localLastOverflow, opMode);
}

// Overflow interrupt for TCA0 - counts the 5 MHz signal derived from the 10 MHz OCXO (divided by 2)
ISR(TCA0_OVF_vect) {
    overflowCount++;
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm; // Clear the overflow interrupt flag
}

// Capture interrupt for TCB0 - PPS event
ISR(TCB0_INT_vect) {
    timerCounterValue = TCA0.SINGLE.CNT;
    lastOverflowCount = overflowCount;
    overflowCount     = 0;
    ppsReady          = true;
    TCB0.INTFLAGS     = TCB_CAPT_bm; // Clear the capture interrupt flag
}

// ADC conversion complete interrupt
ISR(ADC0_RESRDY_vect) {
    ticValue      = static_cast<int>(ADC0.RES);
    adcReady      = true;
    ADC0.INTFLAGS = ADC_RESRDY_bm; // Clear the interrupt flag
}

void setTCA0Count(const uint16_t count) {
    TCA0.SINGLE.CNT = count;
}

void setWarmupTime(const uint16_t seconds) {
    warmupTime      = seconds;
    warmupEndMillis = seconds * 1000;
}

void setDacValue(const uint16_t value) {
    if (value <= DAC_MAX_VALUE) {
        dac.write(value);
    } else {
        Serial2.print(F("Error: DAC value out of range: "));
        Serial2.println(value);
    }
}

float readTemperatureC() {
    return temperature.readTemperatureC();
}

float readOCXOTemperatureC() {
    return temperatureOCXO.readTemperatureC();
}

void setOpMode(const OpMode mode, const int32_t holdValue) {
    if (holdValue > 0 && holdValue <= DAC_MAX_VALUE) {
        calculationController.state().holdValue = holdValue;
    }
    opMode = mode;
    lcdController.setOpMode(opMode);
}

void encoderTick() {
    encoder.tick();
}

void initGps() {
    Serial2.println(F("Initializing GPS module with custom configuration..."));
    delay(500);

    // I think this is looping for a long time giving the impression it hangs
    //while (Serial1.available()) Serial1.read();

    // Disable GSV (Satellite list)
    Serial1.println("$PUBX,40,GSV,0,0,0,0,0,0*59");
    // Disable GSA (DOP/Active Satellites)
    Serial1.println("$PUBX,40,GSA,0,0,0,0,0,0*4E");
    // Disable VTG (Track/Speed)
    Serial1.println("$PUBX,40,VTG,0,0,0,0,0,0*5E");
    // Disable GLL (Geographic Position)
    Serial1.println("$PUBX,40,GLL,0,0,0,0,0,0*5C");

    Serial1.flush();

    Serial2.println(F("GPS initialized with custom configuration"));
}

void resetGPS() {
    digitalWriteFast(RESET_GPS, HIGH);
    delay(50);
    digitalWriteFast(RESET_GPS, LOW);
    initGps();
}

void processInputs() {
    rotaryButton.read();
    buttonA.read();
    buttonB.read();
    encoder.tick();

    const int newEncoderPosition = encoder.getPosition(); // NOLINT(*-narrowing-conversions)
    if (encoderPosition != newEncoderPosition) {
        const int diff = newEncoderPosition - encoderPosition;
        lcdPage        = (lcdPage + diff + lcdController.pageCount()) % lcdController.pageCount();
        lcdController.update(lcdPage);
    }
    encoderPosition = newEncoderPosition;
}

void initPinsAndLeds() {
    pinMode(BUTTON_A, INPUT_PULLUP);
    pinMode(BUTTON_B, INPUT_PULLUP);
    pinMode(ROTARY_PUSH, INPUT_PULLUP);
    pinMode(ROTARY_A, INPUT_PULLUP);
    pinMode(ROTARY_B, INPUT_PULLUP);

    pinMode(RESET_GPS, OUTPUT);
    digitalWriteFast(RESET_GPS, LOW);

    pinMode(LOCK_LED, OUTPUT);
    pinMode(HEATING_LED, OUTPUT);
    pinMode(PPS_ERROR_LED, OUTPUT);
    digitalWriteFast(LOCK_LED, LOW);
    digitalWriteFast(HEATING_LED, HIGH);
    digitalWriteFast(PPS_ERROR_LED, LOW);

    pinMode(TIC, INPUT);
    pinMode(PPS_IN, INPUT);
    pinMode(OCXO_IN, INPUT);
}

void initI2CDevices() {
    Wire.begin();
    Wire.setClock(400000UL);

    if (lcd.begin(20, 4) != 0) {
        Serial2.println(F("Error: LCD not detected"));
    } else {
        lcd.clear();
        Serial2.println(F("LCD initialized successfully"));
        lcd.setCursor(0, 0);
        lcd.print(F("LCD initialized successfully"));
    }

    if (!eeprom.begin()) {
        Serial2.println(F("Error: EEPROM not detected"));
    } else {
        Serial2.println(F("EEPROM initialized successfully"));
    }

    if (!dac.begin()) {
        Serial2.println(F("Error: DAC not detected"));
    } else {
        Serial2.println(F("DAC initialized successfully"));
    }
}

void initUserInputs() {
    buttonA.begin();
    buttonB.begin();
    rotaryButton.begin();

    attachInterrupt(ROTARY_A, encoderTick, CHANGE);
    attachInterrupt(ROTARY_B, encoderTick, CHANGE);
}

void initEventsAndTimers() {
    // Configure Event System Channel 0: PA5 (5 MHz, derived from 10 MHz OCXO / 2) -> TCA0
    EVSYS.CHANNEL0 = EVSYS_GENERATOR_PORT0_PIN5_gc; // Route PA5 to Event Channel 0
    EVSYS.USERTCA0 = EVSYS_CHANNEL_CHANNEL0_gc; // Connect Channel 0 to TCA0

    // Configure TCA0 as event counter for the 5 MHz divided OCXO signal
    TCA0.SINGLE.CTRLA    = 0; // Disable TCA0 for configuration
    TCA0.SINGLE.CTRLD    &= ~TCA_SINGLE_SPLITM_bm; // Ensure single mode (not split)
    TCA0.SINGLE.CTRLB    = TCA_SINGLE_WGMODE_NORMAL_gc; // Set normal waveform generation mode
    TCA0.SINGLE.EVCTRL   = TCA_SINGLE_EVACT_POSEDGE_gc | TCA_SINGLE_CNTEI_bm; // Count on rising edges via event input
    TCA0.SINGLE.PER      = 49999; // Set period for modulo-50000 counting
    TCA0.SINGLE.CNT      = 0; // Initialize counter to zero
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm; // Clear any pending overflow interrupt
    TCA0.SINGLE.INTCTRL  = TCA_SINGLE_OVF_bm; // Enable overflow interrupt
    TCA0.SINGLE.CTRLA    = TCA_SINGLE_ENABLE_bm; // Enable TCA0

    // Configure Event System Channel 1: PA4 (PPS) -> TCB0
    EVSYS.CHANNEL1 = EVSYS_GENERATOR_PORT0_PIN4_gc; // Route PA4 to Event Channel 1
    EVSYS.USERTCB0 = EVSYS_CHANNEL_CHANNEL1_gc; // Connect Channel 1 to TCB0

    // Configure TCB0 for input capture on PPS edge
    TCB0.CTRLA    = 0; // Disable TCB0 for configuration
    TCB0.CTRLB    = TCB_CNTMODE_CAPT_gc; // Set input capture mode
    TCB0.EVCTRL   = TCB_CAPTEI_bm | TCB_EDGE_bm | TCB_FILTER_bm;
    TCB0.INTFLAGS = TCB_CAPT_bm; // Clear any pending capture interrupt
    TCB0.INTCTRL  = TCB_CAPT_bm; // Enable capture interrupt
    TCB0.CTRLA    = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm; // Enable TCB0 with CLK/1 prescaler

    // Configure ADC0 for hardware event-triggered conversion
    EVSYS.USERADC0 = EVSYS_CHANNEL_CHANNEL1_gc; // Connect Channel 1 (PPS) to ADC0 start trigger

    // Configure voltage reference before ADC
    VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc; // Select 1.1V internal reference — matches ADC_VREF in Constants.h
    VREF.CTRLB = VREF_ADC0REFEN_bm; // Enable ADC0 voltage reference

    // Configure ADC0 for event-triggered conversion on PDO (PD0/AIN0)
    ADC0.CTRLA = 0; // Disable ADC0 for configuration
    ADC0.CTRLC = ADC_PRESC_DIV16_gc | ADC_REFSEL_INTREF_gc; // Set ADC clock prescaler to CLK/16, Select internal voltage reference NOLINT(*-suspicious-enum-usage)
    ADC0.CTRLD = ADC_INITDLY_DLY0_gc; // Set 16 cycle initial delay for reference settling
    ADC0.SAMPCTRL = 10; // Set sample duration to 10 ADC clock cycles
    ADC0.MUXPOS = ADC_MUXPOS_AIN0_gc; // Select AIN0 (PD0)
    ADC0.EVCTRL = ADC_STARTEI_bm; // Enable event-triggered conversion start
    ADC0.INTFLAGS = ADC_RESRDY_bm; // Clear any pending result ready interrupt
    ADC0.INTCTRL = ADC_RESRDY_bm; // Enable result ready interrupt
    ADC0.CTRLA = ADC_ENABLE_bm | ADC_RESSEL_10BIT_gc; // Enable ADC0 with 10-bit resolution

    // setting interrupt priorities
    // only one vector can have the highest priority
    // in this case we want TCB0 (PPS capture) to have the highest priority
    // The default priority for CPUINT.LVL0VEC
    CPUINT.LVL1VEC = TCB0_INT_vect_num; // set highest priority for TCB0 interrupt
    sei(); // Enable global interrupts
}

void setup() {
    Serial2.begin(115200);

    initPinsAndLeds();

    Serial1.begin(9600);

    initI2CDevices();
    initUserInputs();
    initEventsAndTimers();

    initGps();

    RSTCTRL.RSTFR |= RSTCTRL_WDRF_bm;
    wdt_enable(WDT_PERIOD_8KCLK_gc);

    warmupEndMillis = millis() + warmupTime * 1000UL;

    externalEepromController.begin();
    const EEPROMState eepromState = externalEepromController.loadState();
    calculationController.setEEPROMState(eepromState);
    if (externalEepromController.isValid()) {
        Serial2.print(F("Warm boot: EEPROM state loaded — dacValue="));
        Serial2.print(eepromState.dacValue);
        Serial2.print(F(", iAccumulator="));
        Serial2.println(eepromState.iAccumulator, 4);
    } else {
        Serial2.print(F("Cold boot: no valid EEPROM bank — using defaults, dacValue="));
        Serial2.print(eepromState.dacValue);
        Serial2.print(F(", iAccumulator="));
        Serial2.println(eepromState.iAccumulator, 4);
    }
}

void processGps() {
    while (Serial1.available() > 0) {
        const int c = Serial1.read();
        gps.encode(static_cast<char>(c));
        lastGpsReceiveMillis = millis();
    }
    if (gps.location.isValid() && gps.location.isUpdated()) {
#ifdef DEBUG_GPS
        Serial2.print(F("GPS position updated: "));
        Serial2.print(gps.location.lat(), 6);
        Serial2.print(F(", "));
        Serial2.println(gps.location.lng(), 6);
#endif
        lcdController.gpsData().latitude        = gps.location.lat();
        lcdController.gpsData().longitude       = gps.location.lng();
        lcdController.gpsData().isPositionValid = true;
    }

    if (gps.satellites.isValid() && gps.satellites.isUpdated()) {
#ifdef DEBUG_GPS
        Serial2.print(F("GPS satellites updated: "));
        Serial2.println(gps.satellites.value());
#endif
        lcdController.gpsData().satellites        = gps.satellites.value();
        lcdController.gpsData().isSatellitesValid = true;
    }

    if (gps.date.isValid() && gps.date.isUpdated()) {
#ifdef DEBUG_GPS
        Serial2.print(F("GPS date updated: "));
        Serial2.print(gps.date.year());
        Serial2.print(F("-"));
        Serial2.print(gps.date.month());
        Serial2.print(F("-"));
        Serial2.println(gps.date.day());
#endif
        lcdController.gpsData().year        = gps.date.year();
        lcdController.gpsData().month       = gps.date.month();
        lcdController.gpsData().day         = gps.date.day();
        lcdController.gpsData().isDateValid = true;
    }

    if (gps.time.isValid() && gps.time.isUpdated()) {
#ifdef DEBUG_GPS
        Serial2.print(F("GPS time updated: "));
        Serial2.print(gps.time.hour());
        Serial2.print(F(":"));
        Serial2.print(gps.time.minute());
        Serial2.print(F(":"));
        Serial2.print(gps.time.second());
        Serial2.print(F("."));
        Serial2.println(gps.time.centisecond());
#endif
        lcdController.gpsData().hour        = gps.time.hour();
        lcdController.gpsData().minute      = gps.time.minute();
        lcdController.gpsData().second      = gps.time.second();
        lcdController.gpsData().centisecond = gps.time.centisecond();
        lcdController.gpsData().isTimeValid = true;
    }

    // GPS watchdog: reset if no data received within timeout
    if (lastGpsReceiveMillis != 0 && (millis() - lastGpsReceiveMillis > GPS_RESET_TIMEOUT_MS) &&
        (millis() - lastGpsResetMillis > GPS_RESET_TIMEOUT_MS)) {
        Serial2.println(F("GPS watchdog: resetting GPS due to timeout"));
        resetGPS();
        lastGpsResetMillis = millis();
    }
}

void processCommands() {
    // todo temp just for setting dac value
    // create a commandProcessor if this is going to be a thing
    if (Serial2.available() > 0) {
        const int read = Serial2.read();
        if (read == 'd') {
            const uint16_t dacValue = Serial2.parseInt();
            const float dacVoltage  = static_cast<float>(dacValue) / DAC_MAX_VALUE * DAC_VREF;
            Serial2.print(F("Setting DAC value: "));
            Serial2.print(dacValue);
            Serial2.print(F(", Voltage: "));
            Serial2.print(dacVoltage, 4);
            setDacValue(dacValue);
            calculationController.state().dacValue   = dacValue;
            calculationController.state().dacVoltage = dacVoltage;
        } else if (read == 'i') {
            externalEepromController.invalidate();
            Serial2.println(F("EEPROM state invalidated — next boot will be a cold start with default values"));
        } else if (read == 's') {
            manuallySaveState();
        }

        while (Serial2.available() > 0) {
            Serial2.read(); // flush rest of line
        }
    }
}

void loop() {
    lcdController.setOpMode(opMode);
    lcdController.update(lcdPage);
    processGps();
    processInputs();
    processCommands();

    if (opMode == WARMUP && millis() > warmupEndMillis) {
#ifdef DEBUG
        Serial2.println(F("Heater warm-up complete"));
#endif
        opMode = RUN;
        digitalWriteFast(HEATING_LED, LOW);
    }

    // missed PPS
    // overflowCount will reset automatically once a PPS is received
    if (opMode != WARMUP && !ppsError && overflowCount > 130) {
        ppsError = true;
        digitalWriteFast(PPS_ERROR_LED, HIGH);
    }

    if (ppsReady && adcReady) {
#ifdef DEBUG
        Serial2.println(F("===== PPS Event ====="));
        Serial2.print(F("Captured Pulse Count: "));
        Serial2.println(timerCounterValue);
        Serial2.print(F("ADC Reading (PD0): "));
        Serial2.println(ticValue);
        Serial2.print(F("Overflow Count: "));
        Serial2.println(lastOverflowCount);
#endif
        ppsReady = false;
        adcReady = false;

        if (ppsError) {
            ppsError = false;
            digitalWriteFast(PPS_ERROR_LED, LOW);
        }
        doCalculation();
        digitalWriteFast(LOCK_LED, calculationController.state().ppsLocked ? HIGH : LOW);
        if (manualSaveRequested && millis() - lastManualSaveMillis > 5000) {
            saveState(calculationController.getEEPROMState());
            Serial2.println(F("Controller state manually saved to EEPROM"));
            lastManualSaveMillis = millis();
            manualSaveRequested  = false;
        }
    }

    wdt_reset();
}
