# Watchdogs

Generated: 2026-01-26

This document describes the two watchdog mechanisms used (or recommended) in the firmware:

- GPS watchdog — resets the external GPS module when the GPS stops sending data.
- MCU (hardware) watchdog — a hardware watchdog that resets the microcontroller if the firmware hangs.

Keep this file in `docs/` so the behaviour and safety considerations are recorded next to the code.


## GPS watchdog

Purpose
- Detect absence of GPS data and perform a targeted reset of the GPS module (toggle RESET_N) so the GPS may recover without resetting the MCU.

Where implemented
- Primary implementation location: `src/main.cpp`.
- Key symbols (variable and function names used by the firmware):
  - `lastGpsReceiveMillis` — timestamp (millis) of the last time GPS bytes were read.
  - `lastGpsResetMillis` — timestamp (millis) of the last GPS reset performed.
  - `GPS_RESET_TIMEOUT_MS` — configured timeout for GPS inactivity (default in repo: `60000UL` — 60 seconds).
  - `resetGPS()` — function that pulses the `RESET_GPS` pin to reset the GPS module. In the current code this pulses the MCU pin for a short period.

Behavior
- Whenever the code reads bytes from the GPS serial port (`Serial1`) it updates `lastGpsReceiveMillis`.
- If `lastGpsReceiveMillis != 0` (we've received at least one byte) and `millis() - lastGpsReceiveMillis > GPS_RESET_TIMEOUT_MS` and `millis() - lastGpsResetMillis > GPS_RESET_TIMEOUT_MS`, the watchdog calls `resetGPS()` and records `lastGpsResetMillis`.
- The extra `lastGpsResetMillis` guard throttles repeated resets so the GPS is not spam-reset when it cannot recover.

Why bytes vs "valid sentence"
- The current implementation treats receipt of any byte as evidence the GPS is alive.
- This is simple and works for most modules, but can be fooled by electrical noise or malformed data.
- Optional stricter approach: update `lastGpsReceiveMillis` only when TinyGPSPlus reports a meaningful change (e.g., `gps.time.isUpdated()`, `gps.location.isUpdated()`, or `gps.satellites.isUpdated()`). This reduces false positives.

Reset polarity and transistor wiring
- The MAX‑M10S datasheet documents `RESET_N` as active low and requires it to be held low for at least 1 ms to trigger a reset (datasheet: MAX‑M10S). The relevant requirement:
  - `RESET_N` (active low) must be low for at least 1 ms to trigger a reset.
- If you have an NPN transistor between the MCU pin and the module's `RESET_N` pin (common low-side driver arrangement), the MCU's output is inverted by the transistor:
  - MCU LOW -> NPN off -> `RESET_N` pulled HIGH by module (normal operation)
  - MCU HIGH -> NPN on  -> `RESET_N` pulled LOW (reset asserted)
- Given the inversion, the firmware must drive the MCU pin HIGH to assert a LOW on `RESET_N`.
- The code in `resetGPS()` currently drives the MCU pin HIGH for some milliseconds and then returns it LOW. This matches the inverted wiring model and the datasheet requirement.

Recommended pulse width
- The datasheet minimum is 1 ms; the current code used 10 ms. To build in margin and improve reliability across modules and power/cleanup delays, we strongly recommend 50–100 ms instead of 10 ms.
- Example change (in `resetGPS()`):
  ```cpp
  digitalWriteFast(RESET_GPS, HIGH); // if using NPN inverter, this asserts RESET_N low
  delay(100);                         // 100 ms safe margin
  digitalWriteFast(RESET_GPS, LOW);  // release reset
  ```

Blocking vs non-blocking reset
- Current implementation uses `delay()` which blocks the loop for the pulse duration. This is acceptable because resets are rare and short, but if you prefer the main loop to keep running during the pulse, consider a non-blocking implementation which sets a `gpsResetActive` flag and releases the reset after the pulse duration in `loop()`.

Throttling and escalation
- A conservative approach (recommended):
  - Attempt targeted GPS reset as described above. If the GPS recovers, normal operation resumes.
  - If the GPS does not recover after a small number of reset attempts (for example 3 attempts within 10 minutes), escalate to a full MCU reboot via the hardware watchdog (see MCU watchdog section). This prevents endless GPS reset loops and ensures a full system restart when necessary.

Testing instructions (hardware)
1. With the system running and serial monitor open for debug output (`Serial2`), disconnect the GPS TX line or otherwise stop the GPS from sending data.
2. Wait `GPS_RESET_TIMEOUT_MS` (default 60 s). The firmware should log a watchdog message and pulse the GPS reset pin.
3. Observe the GPS module resume output (NMEA sentences or UBX startup messages) after the reset. If not, increase pulse width to 100 ms and re-test.
4. To speed testing, temporarily change `GPS_RESET_TIMEOUT_MS` to a small value (e.g., 5000UL) and re-flash.

Troubleshooting
- If reset pulses are not being seen on the GPS `RESET_N` pin:
  - Verify transistor wiring (emitter to GND, collector to GPS RESET_N, base to MCU pin through resistor) and verify that GPS RESET_N has a pull-up to Vcc.
  - With a scope or logic analyzer, probe the MCU pin and the GPS RESET_N pin to confirm the expected inversion and pulse width.
  - If the transistor is not saturating (RESET_N not pulled fully low), check base resistor value and MCU logic level.


## MCU (hardware) watchdog

Purpose
- Detect firmware hangs or deadlocks and reset the microcontroller automatically to recover to a known-good state.

Implementation notes (current, simple approach)
- The simplest approach is to call `wdt_enable(...)` in `setup()` and call `wdt_reset()` regularly in `loop()`.
- In your repo this has been implemented in `src/main.cpp` in the simple form: enable WDT in `setup()` and call `wdt_reset()` near the end of `loop()`. That prevents the WDT from resetting the MCU so long as the main loop is running normally.

Example (simple)
```cpp
// in setup()
RSTCTRL.RSTFR |= RSTCTRL_WDRF_bm ;
wdt_enable(WDT_PERIOD_8KCLK_gc);

// in loop()
wdt_reset(); // pet the watchdog
```

Notes and important caution
- WDT resets the MCU (not the GPS module). If you want to reset the GPS module specifically, use the GPS watchdog described above.
- If you want an escalation strategy where the MCU is rebooted after repeated failed GPS resets, you can use the WDT for that purpose:
  - Do your GPS reset attempts first. If they fail N times within M minutes, stop calling `wdt_reset()` (or enable the WDT with a short timeout) so the WDT resets the MCU.
  - Be careful: some AVR cores maintain the WDT across reset. Ensure `setup()` handles any legacy WDT state appropriately (some cores require clearing the WDT early in `setup()` to avoid immediate resets).
- Choose a WDT timeout that balances ability to recover from transient stalls and avoiding unintended resets during legitimate long operations. 1–4 seconds is common; choose based on your system.

Testing the MCU watchdog
- To force a WDT reset during testing, temporarily remove or comment out the `wdt_reset()` call in `loop()` and let the MCU hang; it should reset after the configured watchdog timeout. Use `Serial2` messages and an LED to confirm the reboot.


## Integration and recommended escalation policy (example)
1. Normal operation: `wdt_reset()` is called regularly in `loop()` so the MCU watchdog is not triggered.
2. If no GPS bytes are received for `GPS_RESET_TIMEOUT_MS`, perform a targeted `resetGPS()` (one reset). Record timestamp and increment a `gpsResetCount`.
3. If GPS does not recover after `N` resets within some time window (for example N=3 within 10 minutes), escalate to MCU reboot:
   - Option A: stop calling `wdt_reset()` and let hardware watchdog reset the MCU (simple).
   - Option B: explicitly trigger a software reset (if supported) after preparing shutdown steps.
4. On MCU restart, the firmware should reinitialize peripherals and resume normal behaviour. Optionally store a persistent reboot count in EEPROM for diagnostics.


## Quick reference (what to change)
- `GPS_RESET_TIMEOUT_MS` — change the GPS inactivity timeout in `src/main.cpp` to tune behavior.
- `resetGPS()` — increase the delay from 10 ms to 100 ms for extra margin.
- `wdt_enable(...)` / `wdt_reset()` — ensure the `wdt_reset()` call remains in `loop()` so the WDT is only used as a last-resort recovery.


## Next steps and TODOs
- (Optional) Modify GPS watchdog to update `lastGpsReceiveMillis` only on meaningful TinyGPSPlus updates (e.g., `gps.time.isUpdated()`) to avoid false resets from noise.
- (Optional) Add a persistent `gpsResetCount` in EEPROM to help diagnose flaky GPS modules in the field.
- (Optional) Implement the escalation policy to let the WDT reset the MCU after repeated GPS module resets.


If you want, I can apply the safe `resetGPS()` pulse change (10 ms -> 100 ms) and/or convert the reset call to a non-blocking implementation. Which would you like me to do next?
