// ReSharper disable CppMemberFunctionMayBeStatic
#include <CommandProcessor.h>
#include <../archive/EEPROMController.h>

CommandProcessor::CommandProcessor(Stream& serialPort,
                                   const SetDacFn setDac,
                                   const SetWarmupTimeFn setWarmupTime,
                                   const SetOpModeFn setOpMode,
                                   const ManuallySaveStateFn manuallySaveState,
                                   CalculationController& calculationController,
                                   SerialOutputController& serialOutputController,
                                   ExternalEEPROMController& eepromController)
    : serial_(serialPort),
      setDac_(setDac),
      setWarmupTime_(setWarmupTime),
      setOpMode_(setOpMode),
      manuallySaveState_(manuallySaveState),
      controller_(calculationController),
      serialOutputController_(serialOutputController),
      eepromController_(eepromController) {
}

void CommandProcessor::printHelp() const {
    serial_.println(F("Available commands:"));
    serial_.println(F("? : this help"));
    serial_.println(F("d or D<value> : set DAC value (0 to 65535)"));
    serial_.println(F("h or H<value> : set operation mode to HOLD with hold value (1 to 65535)"));
    serial_.println(F("  a value of 0 does not change the hold value"));
    serial_.println(F("i or I : invalidate persisted controller state"));
    serial_.println(F("l or L<value> : set TIC linearization parameters"));
    serial_.println(F("p or P[s|c|l|t] : print status"));
    serial_.println(F("  s : summary (default)"));
    serial_.println(F("  c : control state"));
    serial_.println(F("  l : long term state"));
    serial_.println(F("  t : long term trail"));
    serial_.println(F("r or R : set operation mode to RUN"));
    serial_.println(F("s or S : save controller state to EEPROM"));
    serial_.println(F("t or T[c|r]<value> : set temperature parameters"));
    serial_.println(F("  c<value> : set temperature coefficient (float)"));
    serial_.println(F("  r<value> : set temperature reference in Celsius (float)"));
    serial_.println(F("u or U<value> : set time constant in seconds (4 to 32000)"));
    serial_.println(F("w or W<seconds> : set warmup time in seconds (1 to 1000)"));
}

void CommandProcessor::setWarmupTime(const uint16_t value) const {
    if (value >= 1 && value <= 1000) {
        setWarmupTime_(value);
        serial_.print(F("Warmup time set to "));
        serial_.print(value);
        serial_.println(F(" seconds"));
    }
    else {
        serial_.println(
            F("Not a valid warmup time - Must be between 1 and 1000 seconds"));
    }
}

void CommandProcessor::setDac(const uint16_t value) const {
    if (value > 65535) {
        serial_.println(F("Not a valid DAC value - Must be between 0 and 65535"));
        return;
    }

    controller_.state().dacOut = value;
    setDac_(value);
    serial_.print(F("DAC value set to "));
    serial_.println(value);
}

void CommandProcessor::process() const {
    enum Command {
        help = '?',
        d = 'd',
        D = 'D', // set dac value command
        h = 'h',
        H = 'H', // set opMode to hold
        i = 'i',
        I = 'I', // invalidate persisted controller state
        l = 'l',
        L = 'L', // set TIC linearization parameters min max square
        p = 'p',
        P = 'P', // print status command
        r = 'r',
        R = 'R', // set opMode to run
        s = 's',
        S = 'S', // save controller state to EEPROM
        t = 't',
        T = 'T', // set temp coefficient or reference
        u = 'u',
        U = 'U', // set time const
        w = 'w',
        W = 'W', // override warmup time
    };

    if (serial_.available() > 0) {
        long value;
        switch (serial_.read()) {
            case d: // set dac value command
            case D:
                value = serial_.parseInt();
                setDac(value); // NOLINT(*-narrowing-conversions)
                break;
            case h:
            case H: // set opMode to hold
                setOpMode_(hold, serial_.parseInt());
                serial_.println(F("Operation mode set to HOLD"));
                break;
            case i:
            case I: // invalidate persisted controller state
                eepromController_.invalidate();
                serial_.println(F("EEPROM persisted state invalidated"));
                //todo I should reset here
                break;
            case help: // help command
                printHelp();
                break;
            case l:
            case L: // set TIC linearization parameters command
                value = Serial.parseInt();
                Serial2.println(F("Setting TIC linearization parameters"));
                if (value >= 1 && value <= 500) {
                    controller_.setTicMin(static_cast<float>(value) / 10.0);
                    Serial2.print(F("TICmin "));
                    Serial2.println(controller_.state().ticMin);
                }
                else if (value >= 800 && value <= 1023) {
                    controller_.setTicMin(static_cast<float>(value));
                    Serial2.print(F("TICmax "));
                    Serial2.println(controller_.state().ticMax);
                }
                else if (value >= 1024 && value <= 1200) {
                    controller_.setX2(static_cast<float>(value - 1000) / 1000.0);
                    Serial2.print(F("square compensation "));
                    Serial2.println(controller_.state().x2);
                }
                else {
                    Serial2.println(F("Not a valid value"));
                }
                break;
            case p: // print status command
            case P:
                switch (serial_.read()) {
                    case 's': // print summary
                    case 'S':
                        serialOutputController_.printSummary();
                        break;
                    case 'c': // print control state
                    case 'C':
                        serialOutputController_.printControlState();
                        break;
                    case 'l': // print long term state
                    case 'L':
                        serialOutputController_.printLongTermState();
                        break;
                    case 't': // print long term trail
                    case 'T':
                        serialOutputController_.printLongTermAll();
                        break;
                    default:
                        serialOutputController_.printSummary();
                        serialOutputController_.printControlState();
                        break;
                }
                break;
            case r:
            case R: // set opMode to run
                setOpMode_(run, 0);
                serial_.println(F("Operation mode set to RUN"));
                break;
            case s:
            case S:
                manuallySaveState_();
                serial_.println(F("Requested controller state save to EEPROM"));
                break;
            case t:
            case T: // set temp coefficient or reference
                switch (serial_.read()) {
                    case 'c': // set temperature coefficient
                    case 'C': {
                        const float coefficient = serial_.parseFloat();
                        controller_.setTemperatureCoefficient(coefficient);
                        serial_.print(F("Temperature coefficient set to "));
                        serial_.println(coefficient);
                    }
                    break;
                    case 'r': // set temperature reference
                    case 'R': {
                        const float temperatureReference = serial_.parseFloat();
                        controller_.setTemperatureReference(temperatureReference);
                        serial_.print(F("Temperature reference set to "));
                        serial_.println(temperatureReference);
                    }
                    break;
                    default:
                        serial_.println(F("No valid sub-command for 't' command"));
                        break;
                }
                break;
            case u:
            case U: // set time const
                value = serial_.parseInt();
                if (value >= 4 && value <= 32000) {
                    controller_.setTimeConstant(static_cast<uint16_t>(value));
                    serial_.print(F("Time constant set to "));
                    serial_.println(value);
                }
                else {
                    serial_.println(F("Not a valid time constant - Must be between 4 and 32000"));
                }

                break;
            case w:
            case W: // set warm up time command
                value = serial_.parseInt();
                setWarmupTime(value);
                break;
            default:
                serial_.println(F("No valid command"));
                break;
        }

        while (serial_.available() > 0) {
            serial_.read(); // flush rest of line
        }
    }
}
