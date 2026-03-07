# Code review: src/ (GPSDO firmware)

This document reviews the code currently under `src/` and gives actionable feedback, prioritized recommendations, and concrete pointers for improvements.

Short plan
- Inspect the main modules in `src/` and evaluate structure, separation of concerns, concurrency, persistence, and coding style.
- Provide per-module comments, cross-cutting concerns, and prioritized next steps.

Checklist (what I covered)
- [x] High-level structure and module boundaries
- [x] Concurrency: ISRs and shared-state handling
- [x] Persistence and EEPROM usage
- [x] Command/CLI and serial I/O
- [x] Maintainability and testability
- [x] Coding-style, naming, and documentation

Summary / overall assessment
----------------------------
- Structure: The code is split into reasonable modules (main wiring, control logic, EEPROM persistence, serial/CLI output). This is a solid starting point and follows the project's migration goal of modularizing the legacy monolith.
- Separation of concerns: Generally good — heavy maths and state live in the CalculationController module, persistence is in `EEPROMController`, and I/O/CLI are separated. `main.cpp` wires hardware and modules.
- Maintainability: Medium. The design choices support maintainability, but a few large functions (notably the controller `calculate()` method) and some code-organization details make ongoing maintenance harder than necessary.
- Concurrency: ISRs are small and snapshot variables are used; `ATOMIC_BLOCK` is applied in main to read multi-byte ISR-shared values — this is correct and important for AVR concurrency.
- Best practices: Many are followed (use of fixed-width types in data structs, EEPROM.update usage, magic value for EEPROM image validity). Some improvements are recommended (smaller functions, centralize configuration, consistent naming, and reduced global state where possible).

Module-level review
-------------------
The review below assumes the `src/` contents include (but may not be limited to):
- `main.cpp`
- `CalculationController.h` / `CalculationController.cpp` (formerly `Controller`)
- `EEPROMController.h` / `EEPROMController.cpp`
- `CommandProcessor.h` / `CommandProcessor.cpp`
- `SerialOutputController.h` / `SerialOutputController.cpp`
- `Callbacks.h`

main.cpp
- Role: hardware initialization (EVSYS/TCA/TCB/ADC setup), ISR wiring, wiring of modules, and the main loop.
- Strengths:
  - Centralized hardware setup and clear comments for each peripheral section.
  - ISRs are minimal; ADC and capture ISR set flags only. This strategy reduces ISR work and latency.
  - Uses `ATOMIC_BLOCK` to obtain snapshots of multi-byte shared variables before calling into the controller.
- Notes / Pointers:
  - `main.cpp` still contains a lot of procedural setup code — this is normal for firmware, but consider encapsulating peripheral initialization into small functions/classes (a thin HAL) to make main shorter and easier to read.
  - Consider adding a short startup diagnostic or safe-mode if hardware initialization fails (e.g., if I2C devices don't ack).

`CalculationController` (previously `Controller`).
- Role: core control algorithm (TIC linearization, PI controller, filtering, DAC calculation, long-term aggregation).
- Strengths:
  - State is encapsulated in `ControlState` and `LongTermControlState` structs which is good for clarity and possible serialization.
  - Calculation logic kept out of ISRs; main loop calls `calculate()` with snapshots.
- Concerns / Pointers:
  - `calculate()` is large and does many responsibilities: TIC linearization, timer updates, lock detection, filter updates, PI math, temperature compensation, DAC finalization, long-term aggregation, persistence decisions, and snapshot saves. Large functions make reasoning, testing, and refactors risky.
  - Suggestion: split `calculate()` into smaller, well-named helper methods (e.g., `updateTimerUs()`, `updatePpsLock()`, `applyFilters()`, `computePi()`, `applyTempCompensation()`, `finalizeDacOutput()`, `updateLongTermAggregates()`). Each helper should have a small contract (inputs/outputs) and be independently verifiable.
  - Consider moving pure computations (no hardware callbacks) into free/static functions so they are easier to inspect and validate offline without AVR dependencies.
  - Numeric safety: the code uses floats and large accumulators; be explicit about expected ranges and clamp early (I know it does some clamping, but centralizing range checks reduces mistakes).

EEPROMController
- Role: pack/unpack persisted `PersistedState`, safe write with magic, `loadState()` and `saveState()` helpers.
- Strengths:
  - Uses a packed struct and writes magic last to avoid partially-valid images — good atomic-like technique.
  - Uses `EEPROM.update()` which avoids unnecessary writes and reduces wear.
- Concerns / Pointers:
  - The saved layout is tightly coupled to internal `ControlState`. That's fine, but document every saved field clearly (you have `docs/eeprom-layout.md`, good). Keep the `kMagic` bump policy documented and enforced.
  - Consider adding versioning fields to PersistedState (e.g., a version byte) to ease future migrations.
  - Consider extracting the raw read/write sequence into small helper functions to consolidate endianness handling and reduce duplicate code.

CommandProcessor
- Role: parse single-character serial commands and call callbacks (set DAC, warmup, save/invalidate EEPROM, print status, set temp params).
- Strengths:
  - Simple design consistent with many embedded CLIs (single-letter commands), minimal parsing.
- Concerns / Pointers:
  - Parsing is byte-oriented and relies on immediate `serial_.read()` calls; it works for simple CLI but can be brittle with slow hosts or line-oriented usage. Consider accepting line-based commands with tokenization (e.g., `mode hold 40000`) especially since there are multi-word commands now.
  - Consider centralizing command definition and help text so adding commands is less error-prone.

SerialOutputController
- Role: format and print diagnostics and state over serial.
- Strengths: compact, focused; uses `const auto& s = controller_.state()` snapshots when printing, avoiding direct global access.
- Pointers:
  - Consider making printing tolerant to controller fields changing during print (snapshotting is used at call-site, but `printX` functions assume stable references). Alternatively, accept a `ControlState` snapshot argument to print functions.

Callbacks.h
- Role: provides prototypes of ISR callbacks and any small hardware helpers.
- Pointer: Keep it minimal — ISRs are a safety-critical area; avoid adding heavy logic there.

Cross-cutting concerns and best practices
----------------------------------------
Concurrency & ISRs
- Good: ISRs are minimal, set flags, and main takes snapshots with `ATOMIC_BLOCK` before calling calculation. This follows good AVR ISR practices.
- Recommendation: Ensure every multi-byte variable accessed from both ISR and main is either atomic or protected with `ATOMIC_BLOCK` when read/written (double-check any remaining globals).

EEPROM & Persistence
- Good: magic value and `EEPROM.update()` usage.
- Improvements:
  - Add a `version` field to `PersistedState` so future layout changes can be handled more gracefully (migration path).
  - Document and centralize the `kEepromBaseAddr` constant usage.

Naming and consistency
- There was a rename of `Controller` → `CalculationController`. Ensure the rename is completed everywhere in the codebase (headers, source files, docs) to avoid confusion. Keep file names aligned with class names (`CalculationController.h/cpp`).
- Use consistent names for instances — currently `calculationController` is used in `main.cpp` (good). Keep naming consistent across modules and docs.

Testing and validation
- Focus on extracting pure calculation parts into small functions so they are easy to inspect and validate offline (for example by running small scripts or comparing deterministic sample traces). Document a few deterministic input traces with expected outputs so manual or scripted verification is straightforward during refactors.

Coding style and small suggestions
- Use a central `src/config.h` or `src/constants.h` for hardware pin names, numeric constants, and magic numbers. This avoids magic numbers scattered in code.
- Prefer placing helper types (e.g., SetDacFn, ReadTempFn) in a header dedicated to callbacks (`Callbacks.h` is already present — keep those typedefs there and include the header where needed).
- Consider making `ControlState` and `LongTermControlState` POD-like and add a `snapshot()` helper if you want to print or pass immutable snapshots to other components.

Documentation
- Good: `docs/` contains `eeprom-layout.md`, and `todo-list.md`. Continue to add small design docs for the controller math (`docs/controller-design.md`) and HAL pin mapping.

Prioritized recommended changes (concrete, ordered)
--------------------------------------------------
1) Refactor `CalculationController::calculate()` into small helpers (high priority)
   - Why: improve readability and reduce risk of functional regressions.
   - How: extract pure computations first. Keep the exact order of math unchanged. Add clear documentation and deterministic sample traces that can be used for manual verification after changes.

2) Add `version` to `PersistedState` and document migration strategy (medium-high)
   - Why: safer future changes and explicit migration.
   - How: add `uint8_t formatVersion` as first field after magic or use separate header location. Implement a migration function if necessary.

3) Encapsulate peripheral initialization into a thin HAL module (medium)
   - Why: simplifies `main.cpp` and isolates hardware changes.
   - How: create `src/hal.cpp/.h` with `initTimers()`, `initAdc()`, `initI2cDevices()` wrappers.

4) Improve CLI parsing to support line-based or multi-token commands (medium)
   - Why: makes it easier to add commands like `mode hold 40000` and to integrate with human operators or scripts.
   - How: buffer a line, tokenize by whitespace, and dispatch by command name.

5) Document deterministic sample traces for controller logic (low-medium)
   - Why: allows rapid offline inspection and parity checks without relying on long hardware runs.
   - How: produce a few example input traces (timer/tic sequences) and expected outcomes and store them in `docs/`.

6) Centralize configuration and constants (low)
   - Why: reduces magic numbers and improves maintainability.
   - How: new header `src/config.h` with pin defines and numeric defaults.

Quality gates (what to run after changes)
----------------------------------------
- `platformio run` — build for `env:ATmega4808`.
- `platformio device monitor` — smoke test serial output on hardware.

Small code-level notes / quick wins
- When printing controller state, prefer taking a snapshot object rather than reading `controller.state()` across multiple prints — this avoids races if fields change while printing.
- Consider adding `static_assert` guards where assumptions exist (size of PersistedState, ranges) — some are already present and are valuable.
- Consider a `Watchdog` and safe start mode as a defensive measure while you continue development.

Concluding remarks
------------------
The project structure is sound and the main architectural decisions are good: modularization, minimal ISRs, an explicit EEPROM layout, and a clear separation between wiring (main) and logic (controller). The top area for improvement is reducing the size and responsibility of `calculate()` and increasing clarity by extracting smaller functions and documenting deterministic traces for verification.

If you want, I can:
- Implement the `calculate()` refactor into smaller private helpers and provide example deterministic traces in `docs/` to verify parity, or
- Add the `formatVersion` field to EEPROM layout and the related migration logic, or
- Create the thin `hal` module and move peripheral initialization out of `main.cpp`.

Which of the above would you like me to do next?
