# GPSDO Firmware - Full Code Review

**Date:** Feb 7, 2026
**Scope:** `src/` directory
**Platform:** ATmega4808 (AVR)

## 1. Executive Summary

The codebase represents a successful migration from a monolithic legacy style (ATmega328p) to a modular, object-oriented architecture for the ATmega4808. The separation of concerns is excellent, with distinct controllers for calculation, display, persistence, and command processing. 

The core control logic from the legacy system appears to be preserved but wrapped in a testable `CalculationController` class. Hardware specifics are largely contained in `main.cpp`, adhering to the goal of abstraction.

**Overall Rating:** High Quality / Maintainable.

---

## 2. Architecture & Design Pattern Analysis

### Modularization
The system is decomposed into five main components:
1.  **`CalculationController`**: Pure logic core. No hardware dependencies (files/registers). Uses dependency injection (function pointers) for I/O.
2.  **`ExternalEEPROMController`**: Handles persistence abstraction.
3.  **`LcdController`**: Handles display logic and paging.
4.  **`CommandProcessor` & `SerialOutputController`**: Handle user interaction.
5.  **`main.cpp`**: The "Composition Root". Handles hardware setup (ISR, Timers, ADC) and wires the components together.

**Pros:**
*   **Testability:** `CalculationController` can be unit-tested on a PC easily because it doesn't depend on `Arduino.h` hardware registers, just types.
*   **Clarity:** It is easy to find where specific logic lives.

### Concurrency
*   **ISRs**: Minimal logic in ISRs (`TCA0_OVF`, `TCB0_INT`, `ADC0_RESRDY`). They capture state into `volatile` variables.
*   **Synchronization**: `main.cpp` uses `ATOMIC_BLOCK` to snapshot these volatile variables before passing them to the main loop processing (`doCalculation`). This is the correct approach to avoid torn reads on 8-bit architectures.

### Persistence Strategy
*   **Wear Leveling**: The `ExternalEEPROMController` implements a sophisticated banking system (8 banks of 2KB).
*   **Safety**: Uses Magic Number + Sequence Counter to always load the freshest valid state. This is robust against power loss during writes.

---

## 3. detailed File-by-File Review

### `main.cpp`
*   **Hardware Setup**: Correctly uses ATmega4808-specific registers (`EVSYS`, `TCA0`, `TCB0`, `ADC0`) to set up the 1PPS measurement pipeline.
*   **Watchdog**: `wdt_enable(WDT_PERIOD_8KCLK_gc)` provides ~8s timeout (assuming 1kHz clock) or similar. `wdt_reset()` is called in the loop.
*   **GPS Processing**: `processGps()` is non-blocking. Includes a software watchdog (`GPS_RESET_TIMEOUT_MS`) to reset the GPS module if data stops flowing.
*   **Loop**: Clean. Calls `update`, `process`, and `doCalculation`.

### `CalculationController` (.h/.cpp)
*   **Core Logic**: Encapsulates the PI loop and FLL (Frequency Locked Loop).
*   **Numerics**: Uses `float` for PI terms. On 8-bit AVR, this is slow but acceptable given the 1Hz update rate. 
*   **Heuristics**: `preloadHeuristic` uses a constant `TCA0_PRELOAD_COUNT`. This suggests a specific hardware timer behavior is expected.
*   **Cleanliness**: Large `calculate` function is broken down into private helpers (`ticLinearization`, `piLoop`, etc.), which improves readability significantly.

### `ExternalEEPROMController` (.h/.cpp)
*   **Logic**: The scan-on-boot strategy (read all headers, find highest sequence) is simple and effective.
*   **Buffer**: Uses a file-scoped static buffer `g_payloadBuf` to avoid stack overflow or dynamic allocation. Good choice for embedded.
*   **Safety**: Explicit `writeLE32` ensures endianness consistency, though AVR is Little Endian anyway. Good practice.

### `LcdController` (.h/.cpp)
*   **Efficiency**: Only updates LCD if `millis()` implies a refresh or page changes.
*   **Formatting**: Custom `formatDMS` handles coordinate display nicely.

### `CommandProcessor` (.h/.cpp)
*   **Interface**: Simple single-character command parser.
*   **Safety**: Basic bounds checking on inputs (e.g., DAC values).

---

## 4. Key Findings & Recommendations

### Minor Issues / Nitpicks
1.  **Function Pointers in Constructor**: `CalculationController` takes raw function pointers (`SetDacFn`, etc.). While valid, C++ interfaces (abstract base classes) represent a more modern/idiomatic approach. However, for a single-target firmware, this is acceptable and avoids vtable overhead (though negligible here).
2.  **Magic Numbers**:
    *   `src/CalculationConstants.h`: `TCA0_PRELOAD_COUNT = 25570`. This value is critical. Ensure it's documented *why* this specific value is chosen (likely related to the 5MHz clock and overflow period).
    *   `src/Constants.h`: `ticOffset = 590`. There is a TODO comment "was 500 -> changed to 590 after updating adc vref". This should be finalized.
3.  **GPS Buffer**: `MAX_GPS_BYTES_PER_LOOP` is 32. If the GPS sends bursts larger than the serial buffer (typically 64 bytes on Arduino), and the loop is slow (due to LCD or EEPROM writes), data *could* be dropped. However, at 9600 baud, 1 byte takes ~1ms. The loop should be fast enough.

### Logic Questions
*   **`CalculationController::updateLongTermState`**:
    *   It saves to EEPROM every 3 hours (`LONGTERM_3H_DIVISOR`).
    *   It uses `saveState_()` callback.
    *   Ensure that the `saveState_` callback (which maps into `ExternalEEPROMController::saveState`) doesn't block for too long, or it might delay the main loop and miss GPS bytes. EEPROM I2C writes are relatively slow (5ms per page typically). Writing a whole struct might take tens of milliseconds.

### Security / Safety
*   **Input Validation**: `CommandProcessor` validates ranges.
*   **Watchdogs**: Both Hardware WDT and GPS software watchdog are present.

### Recommendations using `docs/` guidelines:
1.  **Document the Maths**: The `ticLinearization` polynomial coefficients (`x1, x2, x3`) and the PI loop gains are critical. Ensure `docs/constants.md` explains how these were derived.
2.  **Finalize Constants**: Resolve the TODO in `Constants.h` regarding `ticOffset`.
3.  **Review EEPROM Write Time**: Verify that the 3-hour save (writing ~100+ bytes via I2C) does not cause the 1PPS ISR handling to be delayed significantly or the Serial input buffer to overflow. Implementation looks distinct enough that it shouldn't block interrupts, but `Wire` library interactions can sometimes be blocking.

---

## 5. Conclusion
The codebase is in excellent shape. It adheres to the migration requirements:
*   **No `OriginalCode.cpp` usage**: Logic is fully ported.
*   **ATmega4808 Support**: Native registers used.
*   **Maintainability**: High.

**Ready for**: Functional Testing and hardware validation.

