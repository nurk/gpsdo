# Migration Notes — ATmega328P → ATmega4808

Purpose
-------
This document lists practical differences between the ATmega328P (legacy) and ATmega4808 (target) that are relevant when porting firmware in this repository. It focuses on timers, pins/IO, ADC, DAC, interrupts, and peripheral behaviors that matter for the GPSDO project.

High-level summary
------------------
- ATmega328P: classic AVR (8-bit) with Timer0/1/2, AD converter (single ADC unit), basic external interrupts and no Event System. Arduino cores for Uno/Nano map pins to simple digital numbers.
- ATmega4808: newer AVR (megaAVR 0-series) with richer peripherals: TCA/TCC/TCB timers, Event System (EVSYS), improved ADC with flexible triggers, VREF peripheral, and more fine-grained interrupt priorities. Pin naming in modern cores often uses port/pin macros (e.g. `PIN_PA4`) instead of Arduino D# numbers.

If you only use the Arduino API (digitalRead/attachInterrupt/analogRead) and an Arduino core that targets the 4808, many calls remain portable. The low-level code in this project, however, uses low-level registers and the Event System on the 4808, so careful mapping and review is required.

Detailed differences and migration guidance
-----------------------------------------

1) Timers & capture
- ATmega328P
  - Timers: Timer0 (8-bit), Timer1 (16-bit with Input Capture Unit), Timer2 (8-bit).
  - Input capture typically done on Timer1 using the ICP pin; overflow/compare vectors are `TIMER1_OVF_vect`, `TIMER1_CAPT_vect`.
  - No Event System; external signals feed timers via pin hardware or external interrupt.

- ATmega4808
  - Timers: TCA (16-bit, single 16-bit/dual 8-bit modes, supports event counting), TCB (single compare/capture channel per TCB), TCC (advanced timers on some devices).
  - Event System (EVSYS) allows routing of pin events to timers, ADC, etc. This is used in the code (EVSYS.CHANNELx assignments) to feed the 5 MHz pulse train into TCA0 and PPS into TCB0.
  - Interrupt vector names differ: `TCA0_OVF_vect`, `TCB0_INT_vect` etc.

- Migration tips
  - If migrating from 328P code that used Timer1 input capture, rework to use TCA/TCB + EVSYS on the 4808 (as this project already does).
  - Verify timer clock domains and prescalers. The 4808 timers often run directly from the CPU clock and have different prescaler encodings.
  - Confirm that the counting mode (edge polarity, noise filters) matches your needs — the 4808 provides capture noise filter options on TCB.
  - Check the timer/counter sizes: TCA/TCC are 16-bit (but sometimes have different behaviour) — ensure overflow periods are similar.

2) Event System (EVSYS)
- ATmega328P
  - No integrated Event System. You must connect signals using physical pins, external interrupts, or input capture directly.

- ATmega4808
  - EVSYS allows direct routing between peripherals without CPU involvement (e.g., route PA5 to TCA0 event input, route PA4 to TCB0 capture, route a channel to ADC start).
  - This reduces software jitter and ISR work — used heavily in `src/main.cpp`.

- Migration tips
  - If porting *to* 4808, prefer EVSYS to reduce jitter and ISR overhead. If porting *from* 4808 back to 328P, you must replace EVSYS-based wiring with explicit pin interrupts or hardware capture logic.
  - Validate EVSYS channel assignments and that the example core supports the specific EVSYS generator constants used (e.g., `EVSYS_GENERATOR_PORT0_PIN5_gc`).

3) ADC differences
- ATmega328P
  - ADC registers: ADMUX, ADCSRA, ADCL/ADCH or ADC registers via Arduino API. Reference selection via REFS0/REFS1 bits.
  - Auto-trigger sources: multiple but different names and fewer flexible routing options.
  - Typical developer use: analogRead() or direct ADCL/ADCH reads.

- ATmega4808
  - ADC is more flexible: ADC0 has event-triggered conversions via EVSYS, CTRLA/CTRLC/CTRLD style registers, and the result is read from `ADC0.RES`.
  - Voltage reference is controlled by `VREF` peripheral (`VREF.CTRLA`, `VREF.CTRLB`), not ADMUX.
  - ADC clock prescaler, sample timing, and resolution selection via `ADC0.CTRLC` and `ADC0.CTRLA`.

- Migration tips
  - If you rely on event-triggered ADC start (as this firmware does), keep the EVSYS->ADC routing on the 4808. On 328P you'd implement a software start or use a hardware trigger available for that platform if needed.
  - Verify ADC pin mapping: ADC channel indices differ and the mux configuration is different. E.g., `ADC0.MUXPOS = ADC_MUXPOS_AIN0_gc;` selects a different physical pin than `analogRead(A0)` on an Uno; consult the 4808 datasheet and the chosen Arduino core pin mapping.
  - Validate reference selection: on 4808 `VREF` must be configured (e.g., `VREF_ADC0REFSEL_1V1_gc`), while on 328P you'd set `REFS0`/`REFS1`.
  - Check ADC result reading (`ADC0.RES`) vs ADCL/ADCH on 328P. Also ensure interrupt names (e.g., `ADC0_RESRDY_vect`) match the core headers.

4) DAC
- ATmega328P
  - No built-in DAC. Projects typically use PWM or an external DAC over I2C/SPI.

- ATmega4808
  - The 4808 also doesn't include an integrated DAC in the same way; external DAC usage remains unchanged. This project uses an external DAC8571 over I2C (`DAC8571` library), so no internal-DAC migration is required.

- Migration tips
  - External I2C DACs are portable across MCUs; ensure the I2C (Wire) initialization and pin mapping are correct for the board/core in use.

5) GPIO / pin naming / Arduino core differences
- ATmega328P
  - Many sketches use Arduino D# numbers (e.g., D2) and `attachInterrupt(digitalPinToInterrupt(pin), ...)` and `digitalWrite(pin, value)`.

- ATmega4808
  - Newer cores for 0-series AVRs often expose pin macros like `PIN_PA4` and may provide different digital pin numbering. In this repo the code uses `PIN_PA4`, `PIN_PD0`, etc., which is appropriate for the 4808-oriented core.
  - Beware that `digitalWriteFast` and direct port register use are platform-specific (these may be provided by the chosen core but are not standard Arduino API calls everywhere).

- Migration tips
  - Map logical signals (e.g., PPS input, TIC ADC input, OCXO input) to the schematic pin names and verify correct port/pin macros for your board/core.
  - If you decide to use Arduino D# numbers, consult the core's pin mapping table. Prefer using `PIN_PAx` style in low-level code for clarity when targeting a specific MCU.

6) Interrupt model & priorities
- ATmega328P
  - Fixed priority by vector position; no fine-grained programmable priority.
  - Interrupt vectors: `ISR(TIMER1_OVF_vect)`, etc.

- ATmega4808
  - Provides multiple interrupt levels; software can select vector priority by writing to `CPUINT.LVLxVEC` registers (see code using `CPUINT.LVL1VEC = TCB0_INT_vect_num;` to prioritise PPS capture).
  - Vector names are different (e.g., `TCA0_OVF_vect`, `TCB0_INT_vect`, `ADC0_RESRDY_vect`).

- Migration tips
  - Confirm the interrupt vector names in the core headers and set priorities explicitly if timing-critical events (PPS capture) must outrank others (ADC or slower tasks).
  - Keep ISR bodies small and perform snapshotting with atomic blocks for multi-byte shared data.

7) EEPROM / non-volatile storage
- Both MCUs provide EEPROM with similar byte-addressable access and both are supported by the Arduino `EEPROM` library.
- Differences to watch: total EEPROM size and endurance characteristics may differ slightly. This project stores state starting at address 0 and reserves 100 bytes — verify EEPROM size on the target device if you plan to change the layout.

8) Clocking & oscillator
- Ensure CPU clock speed and prescalers are configured the same (5 MHz pulse capture in this project assumes certain timer clocking). The 4808 typically runs at 20 MHz or another clock depending on fuses or bootloader—confirm core config.

9) Compiler / core differences
- The device headers (register and vector names) differ. Use the correct core/variant in PlatformIO (board/MCU selection) to ensure the register definitions and ISR vector names match.
- Some helper macros/functions (e.g., `digitalWriteFast`) are core-specific. If those are missing in another core, replace them with `digitalWrite` or direct port writes.

Practical checklist for this project
------------------------------------
- [ ] Verify `platformio.ini` or Arduino core is set to a variant that supports ATmega4808 and provides the named registers/macros used in `src/main.cpp` (TCA0, TCB0, EVSYS, VREF, ADC0.*).
- [ ] Cross-check `docs/GPSDO-v1.0.pdf` schematic pin assignments against `#define` pin names in `src/main.cpp` (TIC, PPS_IN, OCXO_IN, LEDs, buttons). Confirm mapping is correct for your board/cable.
- [ ] Confirm EVSYS generator constants used in code (`EVSYS_GENERATOR_PORT0_PIN5_gc`) match the core headers for the chosen device.
- [ ] Validate ADC channel and VREF configuration: ensure `ADC0.MUXPOS = ADC_MUXPOS_AIN0_gc` and `VREF` settings map to the intended physical pin and voltage reference.
- [ ] Test TCA event counting at expected rates (5 MHz). Verify TCA overflow period and that `TCA0.SINGLE.PER = 49999` produces expected timing.
- [ ] Validate ISR priorities: ensure PPS capture ISR (TCB0) has highest priority if needed. The code sets `CPUINT.LVL1VEC = TCB0_INT_vect_num;` — verify the constant exists and behaves as expected in your core.
- [ ] Confirm external I2C DAC works and that `Wire.begin()` uses the correct SDA/SCL pins for your board.

References and further reading
-----------------------------
- ATmega328P datasheet (Timers, ADC, Interrupts) — legacy reference
- ATmega4808 / megaAVR 0-series datasheet — use this for register-level behaviour, EVSYS, ADC0, VREF, TCA/TCB details
- The chosen Arduino core / PlatformIO board variant docs — to map Arduino APIs and pin macros to physical pins and vector names
