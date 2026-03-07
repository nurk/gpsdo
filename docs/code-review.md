# In-depth Code Review — GPSDO_v1.0

Generated: 2026-01-26

This document contains an in-depth code review of the firmware in `src/` with findings, risks, and suggested improvements. It focuses on correctness, maintainability, safety (ISRs and concurrency), EEPROM layout, and hardware-specific code.

Summary
-------
- The project is organized into small controllers (CalculationController, EEPROMController, LcdController, SerialOutputController, CommandProcessor). This separation is good for readability and future unit testing.
- `CalculationConstants.h` centralizes many "magic" numbers used by the control logic — a good step toward transparency.
- The code is overall clear and follows the project's coding guidance (camelCase, fixed-width types used in many places).

High-level recommendations
-------------------------
1. Fix the `LcdController` bug where it holds a copy of `CalculationController` instead of a reference. This causes the LCD to render stale state (copy made at construction). I applied this small, low-risk fix in the codebase.
2. Add atomic/critical-section protection or documented reasoning around non-atomic reads of ISR-updated shared variables that are read outside of ATOMIC_BLOCK (for example `overflowCount` used in loop for PPS-missed detection).
3. Harden serial command parsing: sub-command reads use `serial_.read()` without checking availability; prefer `peek()` or check `available()` before sub-reads to avoid -1 or erroneous values when input is slow/fragmented.
4. Audit ISR-safety of libraries called from ISRs: `encoder.tick()` is called from an ISR; verify the RotaryEncoder::tick implementation is ISR-safe (no allocations, no blocking, no use of non-atomic shared resources). If not ISR-safe, move encoder.tick calls to a high-frequency timer or poll in loop.
5. Document all EEPROM offsets and layout in `docs/eeprom-layout.md` (there is already a file; ensure it reflects `PersistedState` packing and `kEepromBaseAddr`). Mention endianness assumption (little-endian) and MCU-specific packing.
6. Consider a small unit-test harness (host-side) for `CalculationController` pure logic by extracting calculation-only functions and separating hardware callbacks; this will allow CI checks without hardware.

File-by-file findings (high -> low severity)
-------------------------------------------

1) `src/LcdController.h` / `src/LcdController.cpp` — HIGH
- Issue: `LcdController` stores a copy of `CalculationController`:
  - In `LcdController.h` the member is declared as `CalculationController calculationController_;` (a value), but the constructor takes a `CalculationController&` and the `.cpp` initializer uses `calculationController_(calculationController)`.
  - Result: The constructor makes a copy of the controller at boot time. The LCD will display the copied state, which will not be updated by the running controller, making displayed values stale and misleading.
- Impact: High — the UI will show incorrect values.
- Fix: Change the member to a reference: `CalculationController& calculationController_;` so the LCD reads live state. (This fix is implemented.)

2) `src/main.cpp` — MEDIUM
- ISR / shared-state handling:
  - The code correctly marks ISR-shared globals volatile and uses `ATOMIC_BLOCK` when snapshotting `timerCounterValue`, `ticValue`, and `lastOverflowCount` in `doCalculation()`.
  - However, `overflowCount` is read in `loop()` to detect missed PPS (`if (isWarmedUp && !ppsError && overflowCount > 130)`), and this read is not atomic. While `overflowCount` is a `volatile unsigned long` and is incremented in `TCA0_OVF_vect`, concurrent access without an ATOMIC_BLOCK could result in torn reads on 8-bit AVR cores (reading a 32-bit value can be interrupted leading to inconsistent values). Use `ATOMIC_BLOCK(ATOMIC_RESTORESTATE)` around reads of multi-byte ISR-modified variables, or copy them into a local variable within an atomic block before using them.

- Use of attachInterrupt(pin, handler, change) with direct pin defines: confirm the board's Arduino core supports attaching interrupts to pin numbers used here on ATmega4808. Some cores expect `digitalPinToInterrupt(pin)` — but the current usage works on many cores; document this dependency.

- `digitalWriteFast(...)`: this macro may not be present on all cores. The codebase uses it consistently; ensure the core for ATmega4808 in PlatformIO provides it or provide a fallback macro.

3) `src/CalculationController.cpp` / `CalculationConstants.h` — MEDIUM
- Numerical expressions and scaling are complex. Many intermediate computations use floats and then cast to integer types. The code appears faithful to the original algorithm, but keep attention on:
  - Potential overflow of accumulators (e.g., `TIMER_US_BIAS` added to other large values). It appears the constants were tuned for 32-bit signed long, but add comments clarifying safe ranges.
  - The `state_.filterConst` rescaling divides and multiplies older filtered values. If `state_.filterConstOld` is zero (shouldn't happen), guard against division by zero. The code does not check that; consider an assert or constrain to avoid zero.

- Suggestion: Add inline comments describing units for key fields in `ControlState` (e.g., what are the units for `timerUs`, `ticValueFiltered`, `dacValue`). That improves maintainability.

4) `src/EEPROMController.h` / `src/EEPROMController.cpp` — LOW
- Persisted struct `PersistedState` is packed and the code writes/reads raw bytes and uses `kMagic` to validate.
- Good: the code writes the payload first with `magic=0`, then writes the magic last to avoid partially-valid writes — good atomicity pattern on EEPROM.
- Recommendations:
  - Add explicit versioning field in the persisted layout (e.g., `uint8_t version`) so you can evolve the layout without only relying on magic changes.
  - Document endianness and float representation assumptions in `docs/eeprom-layout.md` (which you already have in repo — make sure it matches the struct).

5) `src/CommandProcessor.cpp` — LOW
- Robustness: `process()` calls `serial_.read()` multiple times (for subcommands) without checking `available()` before the secondary reads. This can return -1 or unexpected values when client sends data slowly.
- Recommendation: use `serial_.peek()` to peek the next char (or check `available()`) before consuming it, or read the entire line into a buffer and parse it.

6) ISR use and library safety — MEDIUM
- The code calls `encoder.tick()` from an ISR via `encoderTick()`. Verify the `RotaryEncoder` library's `tick()` method is ISR-safe. If not ISR-safe, `encoder.tick()` should be called in `loop()` at a high-enough frequency or via a short timer interrupt whose handler does nothing but set a flag and the tick occurs in `loop()`.
- Similarly, `ADC0_RESRDY_vect` ISR reads ADC0.RES into `ticValue`; that's fine. The ISRs are short and don't call heavy logic — good.

7) `LcdController` formatting
- `formatDMS()` formats degrees, minutes, seconds. The code uses `\xDF` for the degree symbol. If the LCD uses a different charset, the symbol may appear differently. This is acceptable if tested with the specific LCD; otherwise, consider a safe ASCII fallback.

8) `SerialOutputController::printLongTermAll()` — MEDIUM
- This prints 144 lines and can block for a long time on Serial; that's OK for debug prints but document that it may take multiple seconds and can interfere with timing-sensitive operations if called while the device is disciplining.

Security/safety
---------------
- No network calls or secret handling. EEPROM operations are safe and explicitly invalidated when required.

Testability and CI
------------------
- The code structure (CalculationController separated from hardware) is conducive to host-based unit tests. Consider extracting a smaller pure-logic interface that can be fed deterministic inputs and verified under test.

Small fix I applied
-------------------
- Changed `LcdController::calculationController_` from a value to a reference so the LCD reads live state. This is a low-risk correctness fix and removes the stale-copy bug described above.

Suggested next steps (prioritized)
----------------------------------
1. (High) Verify/guard all multi-byte ISR-updated variables are read/written atomically (use ATOMIC_BLOCK for reads/writes of 16/32-bit variables on 8-bit AVR).
2. (High) Verify library ISR-safety for `RotaryEncoder::tick()` or move encoder handling out of ISR.
3. (Medium) Harden Serial parsing in `CommandProcessor::process()`.
4. (Medium) Add version byte to persisted state and document layout; update `docs/eeprom-layout.md` accordingly.
5. (Medium) Add unit tests for `CalculationController` core logic (host-side harness). This will catch regression in math-heavy code.
6. (Low) Add more comments/units to `Constants.h` `ControlState` struct to clarify units and ranges.

Quality gates checklist
----------------------
- Build: (not executed here) ensure `platformio run` passes. I ran the repository static error check and it reported no immediate syntax/type errors.
- Lint/Style: recommend running any project linters you use; the code follows the repo conventions.
- Unit tests: none currently; add host-side tests for calculation code.

Requirements coverage
---------------------
- Preserve original behaviour: Observed — calculation code appears faithful to `archive/OriginalCode.cpp` (use it as final reference when uncertain).
- Hardware abstraction: callbacks are used for DAC/ADC/timer; good separation.
- Centralize magic numbers: `CalculationConstants.h` already centralizes many constants (Done).

Appendix: concise findings by file
---------------------------------
- `src/main.cpp`: check non-atomic reads of 32-bit `overflowCount`, confirm `attachInterrupt` semantics on ATmega4808, and ensure `digitalWriteFast` availability.
- `src/CalculationController.*`: fine; add guarding comments and check division by zero possible cases.
- `src/LcdController.*`: BUG fixed — changed controller copy to reference.
- `src/EEPROMController.*`: good persisted write pattern; consider versioning.
- `src/CommandProcessor.*`: improve serial parsing robustness.
- `src/SerialOutputController.*`: OK; long prints may block—document.

If you'd like, I can:
- Open a PR with the `LcdController` fix and a small unit test for the controller (host emulation), or
- Implement atomic reads for the `overflowCount` usage and harden `CommandProcessor` serial parsing now.




