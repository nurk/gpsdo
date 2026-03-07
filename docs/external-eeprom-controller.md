External EEPROM controller
==========================

What this document contains
- Short description of how `ExternalEEPROMController` stores and restores state
- On-disk layout and constants
- Usage example (constructor and calls)
- Safety notes and design rationale
- Why this is better than the previous approach

Overview
--------
`ExternalEEPROMController` is a small persistence layer that stores the entire
runtime `ControlState` and `LongTermControlState` into an external I2C EEPROM
(the XBLW 24C128BN device used in this project via the `I2C_eeprom` library).

Key properties
- Uses banking (cyclic slots) to spread writes across the device and reduce
  wear.
- Writes are done conservatively: the payload is written first, the header
  (magic + sequence) is written last. This prevents partially written images
  from being considered valid.
- No dynamic heap allocation at runtime; a single static buffer sized to the
  payload is reused for read/write to keep memory usage deterministic and safe
  for small MCUs.
- The implementation is injected with an `I2C_eeprom&` in the constructor
  (dependency injection) so the controller doesn't rely on a global symbol.

On-disk layout (per-bank)
-------------------------
Each bank (slot) in the external EEPROM contains at its start a small header
followed by the serialized payload.

Header (8 bytes)
- bytes 0..3 : magic (little-endian) 0x47505344
- bytes 4..7 : sequence number (little-endian, monotonically increasing)

Payload (immediately after the header)
- contiguous binary layout of `ControlState` followed by
  `LongTermControlState` (both as POD structs). The controller includes a
  compile-time assert ensuring the payload fits into the configured bank size.

Banking parameters (current code)
- Bank size: 2048 bytes
- Bank count: 8
- Header size: 8 bytes
- Bank base address: 0 (banks are contiguous)

Thus the controller uses 8 * 2KB = 16KB of EEPROM space on the device.

Workflow (what the controller does)
-----------------------------------
- Construction: `ExternalEEPROMController(I2C_eeprom& eeprom)` takes a
  reference to the `I2C_eeprom` instance that performs low-level I2C
  operations.
- begin(): scans all banks to find the
  bank with the highest (valid) sequence number. If found, that bank becomes
  the "active" bank.
- loadState(controller): reads the payload from the active bank into a static
  buffer, reconstructs `ControlState` and `LongTermControlState` via
  memcpy, and assigns them into the provided `CalculationController`.
- saveState(controller): prepares the payload into the static buffer,
  selects the next bank (wraps at the end), writes the payload into the bank
  (after the header), then writes the header with sequence and magic last. The
  controller updates its in-memory `activeBank` and `activeSeq` after a
  successful write.
- invalidate(): clears the header (magic/seq) in every bank so that
  subsequent `begin()` finds no valid state.

Why this design is safer / better
---------------------------------
- Wear leveling: writing to rotating banks spreads writes across the device,
  extending EEPROM life compared to repeatedly writing a single fixed block.
- Atomicity: by writing the header last, consumers never see a partially
  written state as valid; the magic/sequence pair acts as the validity marker.
- Larger storage: the external EEPROM provides far more storage than the
  microcontroller's internal EEPROM, making it possible to persist the full
  `LongTermControlState` (long arrays and aggregates) instead of a trimmed
  snapshot.
- No dynamic heap usage: the code avoids new/malloc in read/write paths and
  instead uses a single static buffer sized exactly to the payload; this
  removes fragmentation and crash modes on small MCUs.
- Encapsulation: header layout, bank size, and count are private constants in
  the class; helper functions are instance methods. There is a compile-time
  assert that ensures the payload fits a bank which prevents accidental
  overflows at build time.

Limitations and notes
---------------------
- The sequence number is a 32-bit unsigned counter. If the device writes more
  than 2^32 updates (extremely unlikely) the sequence will wrap; the code
  selects the bank with the numerically highest sequence during `begin()`.
- The controller relies on the `I2C_eeprom` library's block read/write
  functions (`readBlock`, `updateBlock`) which are already present in this
  project via the PlatformIO dependency.
- Reads/writes to the I2C EEPROM take time; the save path is intended to be
  called occasionally (for example via a user command or periodic snapshot),
  not on every control loop tick.

Usage example (in `main.cpp`)
-----------------------------
- global `I2C_eeprom eeprom(0x50, I2C_DEVICESIZE_24LC128);`
- instantiate controller:

```cpp
ExternalEEPROMController eepromController(eeprom);
```

- init/in use:

```cpp
initI2CDevices(); // calls eeprom.begin()
eepromController.begin();
if (eepromController.loadState(calculationController)) {
  // persisted state loaded
}
...
// save when desired
eepromController.saveState(calculationController);
```

Formatting and recovery
-----------------------
- To force the controller to treat the EEPROM as empty, call
  `eepromController.invalidate()` which clears magic/seq in every bank.
- If you change the persisted struct layout in the future, update the magic
  value or change the layout strategy (and document the new layout in
  `docs/`). The current implementation keeps the magic/seq approach which
  makes detecting incompatible layouts straightforward.


Bank byte-map (visual)
----------------------
Below is a compact visual/byte-map of a single bank. Offsets are relative to
the start of the bank (bank base address + bank_index * bank_size).

Summary:
- Header: 8 bytes (magic + sequence)
- Payload: ControlState then LongTermControlState
- Remaining bytes in the bank (if any) are unused/padding

Human-readable diagram (monospace)

```text
+-------------------------------+--------------------------------+-----------------------------------+
| Offset                        | Length                         | Field                             |
+-------------------------------+--------------------------------+-----------------------------------+
| 0x0000                        | 4                              | MAGIC (0x47505344, little-endian) |
| 0x0004                        | 4                              | SEQ (little-endian sequence)      |
| 0x0008                        | sizeof(ControlState)           | ControlState (binary POD)         |
| 0x0008 + sizeof(ControlState) | sizeof(LongTermControlState)   | LongTermControlState (binary POD) |
| ...                           | remaining                      | padding / unused up to kBankSize  |
+-------------------------------+--------------------------------+-----------------------------------+
```

Compact table (offset expressions)

| Offset expression                         | Bytes expression                                                           | Field |
|-------------------------------------------|---------------------------------------------------------------------------:|:------|
| `0x00`                                    | `4`                                                                        | `MAGIC` (little-endian, 0x47505344) |
| `0x04`                                    | `4`                                                                        | `SEQ` (little-endian, monotonic counter) |
| `0x08`                                    | `sizeof(ControlState)`                                                     | `ControlState` (binary POD) |
| `0x08 + sizeof(ControlState)`             | `sizeof(LongTermControlState)`                                             | `LongTermControlState` (binary POD) |
| end of payload                            | `kBankSize - kHeaderSize - sizeof(ControlState) - sizeof(LongTermControlState)` | padding / unused bytes up to `kBankSize` |

Save cycle duration (estimate)
--------------------------------
This section gives a conservative estimate for how long a complete `saveState()`
cycle will take when writing the payload into the external 24C128 EEPROM.

Assumptions
- I2C clock: 400 kHz (the firmware sets `Wire.setClock(400000UL)`).
- EEPROM page size: 64 bytes (typical for 24C128 / 24LC128 devices).
- EEPROM internal write cycle (t_write): typical ~5 ms, worst-case ~10 ms per page.
- Payload size: `sizeof(ControlState) + sizeof(LongTermControlState)` (the code
  uses this sum as the write length). Example (current build): ~1935 bytes.

How to estimate
1. Number of pages written = ceil(payload_bytes / page_size).
2. Bus transfer time (approx) = payload_bytes / (I2C_bytes_per_second), where
   I2C_bytes_per_second = I2C_clock / 8 (bits -> bytes). At 400 kHz this is 50,000
   bytes/s.
3. EEPROM internal write time = pages * t_write (each page write requires the
   device to commit the page internally and typically takes several ms).
4. Total save time ≈ bus transfer time + EEPROM internal write time + small
   driver overhead.

Worked example (approx)
- payload ≈ 1935 bytes
- pages = ceil(1935 / 64) = 31 pages
- transfer time ≈ 1935 / 50,000 ≈ 0.039 s (≈ 39 ms)
- internal writes (typical) ≈ 31 * 5 ms = 155 ms
- total (typical) ≈ 39 ms + 155 ms ≈ 194 ms (~0.2 s)
- total (worst-case t_write = 10 ms) ≈ 39 ms + 310 ms ≈ 349 ms (~0.35 s)

Notes and guidance
- The header write (magic + seq) is written last; it may require an extra
  internal page write if it modifies bytes in a page that weren't already
  updated by the payload write. The above page count is conservative and
  already accounts for all pages the payload touches; header-only updates
  might add one page in practice.
- The `I2C_eeprom` library may perform page-wise writes and internal polling
  (ack polling) to wait for completion — this is included in the internal
  write time estimate.
- Read (load) time is much faster: reading ~1935 bytes at 400 kHz takes ≈ 40 ms
  on the bus plus small driver overhead, so expect on the order of 50–100 ms
  for a load operation.

Notes about variability
- Actual timings depend on the specific EEPROM device, bus topology, pull-up
  resistors, and I2C driver implementation. The numbers above are good
  conservative estimates for planning and testing.

Summary
-------
`ExternalEEPROMController` is a small, safe, and testable persistence layer
that uses the external I2C EEPROM to reliably store the device runtime state.
It improves upon the legacy approach by adding wear leveling (banking), safer
write ordering, no dynamic allocation, and dependency injection for easier
testing and decoupling.

Notes
-----
- These expressions are intentionally abstract to remain valid even if the structs change. Use the actual compiled `sizeof(...)` values when you need concrete byte offsets (see `src/Constants.h`).
- The controller enforces at compile-time that the sum of payload sizes fits within `kBankSize - kHeaderSize`.
