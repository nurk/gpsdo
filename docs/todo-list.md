Read `.github/copilot-instructions.md` first (it contains onboarding and hard constraints).

Session setup & build
---------------------
- Build: `platformio run`
- Flash: `platformio run -t upload`
- Board/target: see `platformio.ini` (use the configured board variant)
- Important: do not modify `archive/OriginalCode.cpp`. Put long notes in `docs/`.

# TODO: Features and Implementation Tasks

This document collects the remaining work for the GPSDO firmware and presents a navigable, single numbered TODO list with acceptance criteria and implementation guidance.

Table of contents
-----------------
- [~~Task 1 — Serial command: hold~~](#task-1)
- [~~Task 2 — Serial command: run~~](#task-2)
- [~~Task 3 — LCD controller (20x4)~~](#task-3)
- [~~Task 4 — Watchdog and safe startup behaviour~~](#task-4)
- [Task 5 — Runtime limits and hardware safety checks](#task-5)
- [Task 6 — EEPROM wear reduction & atomicity review](#task-6)
- [~~Task 7 — Refactor CalculationController::calculate()~~](#task-7)

---

## Task 1
**Serial command: hold**

- Status: [x] Done

Goal
- Add a CLI command to put the device in hold mode and (optionally) set the hold DAC value in a single command.

Suggested CLI syntax
- `mode hold [<dacValue>]` or `hold [<dacValue>]` (where `dacValue` is optional and if provided must be 0..65535)

Files to change
- `src/CommandProcessor.h` / `src/CommandProcessor.cpp`
- `src/main.cpp`
- (optional) `src/EEPROMController.*` if you want different persistence behaviour

Implementation steps
- Add new command parsing branch in `CommandProcessor::process()`.
- Parse the parameter as optional. If a numeric parameter is present, validate it is within 0..65535 and use it to update `controller.state().holdValue` before entering hold. If no parameter is present, simply set `opMode = hold` and apply the current `controller.state().holdValue`.
- Use the existing `setDacValue` callback to set the physical DAC output after updating `holdValue`.
- Print a confirmation message with the effective DAC value.

Acceptance criteria
- `mode hold 40000` sets the device into hold and updates the DAC to 40000.
- `mode hold` (no value) sets the device into hold and applies the current persisted/active hold value.
- Invalid or out-of-range values produce a helpful error and do not change state.

Notes
- `holdValue` is already persisted by `PersistedState::holdValue` in `src/EEPROMController.h` / `saveState()`.

---

## Task 2
**Serial command: run**

- Status: [x] Done

Goal
- Add a CLI command to set `opMode` to `run` (resuming normal PI control).

Suggested CLI syntax
- `mode run` or `run`

Files to change
- `src/CommandProcessor.h` / `src/CommandProcessor.cpp`
- `src/main.cpp`

Implementation steps
- Add command parsing that recognizes `mode run` and calls a callback in `main.cpp`.
- The callback should set `opMode = run` and call `controller.resetShortTermAccumulators()` if transitioning from hold.
- Print a confirmation message including warmup state and current DAC value.

Acceptance criteria
- `mode run` sets operation mode to `run` and prints confirmation. The command is idempotent.

---

## Task 3
**LCD controller (20x4)**

- Status: [x] Done

Goal
- Provide an `LcdController` that centralizes display logic and shows useful status on the HD44780 20x4 I2C display.

Suggested layout
- Row 1: `time:  00123s  MODE: RUN`
- Row 2: `LOCK: YES  PPSerr: 0  missed: 0`
- Row 3: `TIC: 512 dNs: +12 filt: 500`
- Row 4: `DAC: 32768 T:30.2C W:300s`

Files to add / change
- `src/LcdController.h`, `src/LcdController.cpp`
- Wire into `src/main.cpp` and call update at 1 Hz

Implementation notes
- Provide `update(const ControlState&, const LongTermControlState&, bool isWarmedUp, OperationMode opMode)` and pass snapshots from the main loop.
- Rate-limit to once per second. Use `snprintf` into fixed 21-char buffers per line to avoid dynamic allocation.
- Handle I2C errors gracefully (skip updates if bus error).

Acceptance criteria
- LCD shows readable status and updates without blocking critical timing.

---

## Task 4
**Add watchdog and safe startup behaviour**

- Status: [x] Done

Purpose
- Protect the device from hangs during startup or runtime (I2C/DAC/LCD failures) by using a hardware watchdog and safe degraded startup.

Files / locations
- `src/main.cpp`, optional helper `src/hal_watchdog.h` / `src/hal_watchdog.cpp`, `src/EEPROMController.*` to record reset counts if desired.

Implementation steps
- Configure a watchdog early in `setup()` with a conservative timeout (e.g., 4s). Delay enabling if you need extra time for first-boot initialisation, but aim to enable it as early as possible.
- Pet the watchdog in the main loop and long-running operations.
- When peripheral initialisation fails repeatedly, enter a documented safe mode: stop active control updates, set DAC to last-known-good or safe value, disable LCD updates, and continue petting watchdog to remain alive. Optionally escalate to reset if recovery attempts fail.
- On reset, detect watchdog cause and print diagnostic info; optionally count resets in EEPROM and back off if repeated.

Acceptance criteria
- Watchdog resets the MCU in a deliberate deadlock test. Normal operation does not trigger watchdog resets.
- On peripheral failure the device enters safe mode rather than crashing.

Risks & notes
- Start with a long timeout to avoid accidental reboot loops during development.

---

## Task 5
**Add runtime limits and hardware safety checks**

- Status: [ ] Not started

Purpose
- Prevent the controller from commanding actuator values that could damage the oscillator or downstream hardware, and detect sensor errors.

Files / locations
- `src/CalculationController.cpp`, `src/main.cpp`, `src/CommandProcessor.*`, optional `src/EEPROMController.*` for storing limits.

Implementation steps
- Add configurable min/max DAC limits (defaults: 0..65535). Provide CLI to set/query limits.
- Clamp DAC values to configured bounds before calling `setDacValue` and log clamp events.
- Add sensor fault detection for ADC (stuck values, out-of-range, excess noise) and reduce or disable control until resolved.
- Add a `status` CLI command to report limits and last fault.

Acceptance criteria
- CalculationController never writes DAC values outside configured limits. Sensor faults trigger safe fallback and are reported.

---

## Task 6
**EEPROM wear reduction & atomicity review**

- Status: [ ] Not started

Purpose
- Reduce EEPROM write frequency and ensure persisted writes are atomic and safe to avoid corruption and wear.

Files / locations
- `src/EEPROMController.*` and repo-wide search for `EEPROM.write` usages (including legacy `archive/`).

Implementation steps
- Audit for `EEPROM.write` and prefer `EEPROM.update()` for change-only writes (the current `saveState()` already uses `update()`).
- Keep long-term aggregates saving at 3h boundaries; introduce a cooldown on manual saves (e.g., at most one forced save per 10s).
- Ensure multi-byte groups use the magic-zero-then-write-magic pattern for atomicity; document it.
- Optionally track write counts and warn if they exceed an expected threshold.

Acceptance criteria
- No raw ad-hoc multi-byte write sequences remain that could leave a partially-valid image; write patterns are safe and documented.

---

## Task 7
**Refactor CalculationController::calculate()**

- Status: [x] Done

Purpose
- Split the large `calculate()` into smaller helper functions to improve readability and maintainability while preserving numeric behaviour.

Files / locations
- `src/CalculationController.cpp`, `src/CalculationController.h`.

Implementation steps
- Extract logical units (e.g., TIC linearization, timer update, filtering, PI update, temp compensation, finalize DAC output, long-term state update) into private methods with clear inputs/outputs.
- Keep evaluation order identical. Add instrumentation or small compare harness if desired to validate outputs vs. the original implementation.

Acceptance criteria
- `CalculationController::calculate()` becomes an orchestration method calling small helpers. Numerical behaviour must match within floating-point tolerances.
