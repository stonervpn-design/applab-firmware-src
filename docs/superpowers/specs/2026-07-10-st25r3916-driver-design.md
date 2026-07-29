# ST25R3916 NFC Frontend Driver — Design Spec (HANDOFF)

**Date:** 2026-07-10
**Status:** Approved scope. **HANDOFF to a fresh session** — large NFC-frontend port.

## Goal

Add an **ST25R3916 driver** implementing the port's `furi_hal_nfc` API, so the
**M5Stack NFC Universal Unit (ST25R3916)** works with the firmware. This unblocks
the full NFC capability on the user's actual hardware — normal read/write/emulate,
the mfkey nonce collection (Detect Reader), magic-card writing, and (potentially)
nested/hardnested — all of which the current PN532 backend can't fully do.

## Why this is the real prerequisite

- The port's NFC is **PN532-only**: `components/furi_hal/furi_hal_nfc.c` (1963
  lines) is titled *"NFC HAL implementation for ESP32 with PN532 (I2C)"* and, by
  its own comments, *fakes* an event-driven model because the Flipper NFC stack
  expects an ST25R3916. The board is wired for a PN532/WS1850S over Grove I2C
  (`BOARD_NFC_I2C_ADDR 0x28`, `BOARD_PIN_NFC_IRQ = UINT16_MAX` — no IRQ).
- The **ST25R3916 is the exact chip the real Flipper Zero uses**, so the Flipper's
  driver + the whole NFC protocol stack (already vendored in this port under
  `components/nfc/`) are the native target — porting the frontend makes the mfkey /
  magic / nested reference code work as designed (custom-parity, bit-framing,
  proper card emulation, tight timing) rather than fighting PN532 limits.

## The interface to implement

The ST25R3916 backend must implement the SAME `furi_hal_nfc` API the PN532 adapter
implements (`components/furi_hal/furi_hal_nfc.h`): `furi_hal_nfc_init`,
`_is_hal_ready`, `_acquire`/`_release`, `_set_mode(Poller/Listener)`, tech config
(`Iso14443a/b`, `Iso15693`, `FeliCa`), the **event system** (`FuriHalNfcEvent`
flags: OscOn/FieldOn/FieldOff/ListenerActive/Tx/RxStart-End/Collision/TimerFwt…),
`poller_tx`/`rx`, `listener_tx`/`rx`, custom-parity / short-frame (`0x40`/`0x43`
magic), and the FWT/block timers. The upper NFC stack (`components/nfc/…`) is
unchanged — it already targets this API.

## Reference source (vendor + adapt)

- **Flipper's ST25R3916 register driver + RFAL-lite:** the official flipperzero
  firmware `lib/drivers/st25r3916*.c/.h` and `targets/f7/furi_hal/furi_hal_nfc*.c`
  (the `furi_hal_nfc_i.c` event/transceive core). GPLv3. This is the canonical
  implementation of exactly this API for exactly this chip. Vendor it and swap the
  STM32 SPI/GPIO/timer calls for ESP32 equivalents.

## The ESP32 integration deltas (the real work)

1. **Bus:** the ST25R3916 is natively **SPI** (Flipper uses SPI). Confirm the
   M5Stack Universal Unit's interface — if it exposes **SPI** (likely via the M5
   bus), wire it to an ESP32 SPI host; if the unit is **I2C**, the ST25R3916's I2C
   mode works but is slower — implement whichever the unit provides. Replace the
   Flipper `furi_hal_spi_*` calls with ESP-IDF `spi_device_transmit` (or `i2c_*`).
2. **IRQ (essential):** the ST25R3916 is event-driven via its IRQ pin → the whole
   `FuriHalNfcEvent` model depends on it. Wire the unit's IRQ to a free ESP32 GPIO
   (the board currently has `BOARD_PIN_NFC_IRQ = UINT16_MAX` — assign a real pin),
   install a GPIO ISR that reads the ST25R3916 IRQ registers and raises the
   corresponding `FuriHalNfcEvent` flags to the waiting NFC thread (via a
   FuriEventFlag / thread notification). This replaces the PN532 adapter's
   synchronous-polling fakery with the real interrupt model.
3. **Timers:** map the ST25R3916's internal FWT/GPT timers (it has them on-chip) to
   the `FuriHalNfcEventTimerFwtExpired` events — mostly on-chip, minimal ESP32 timer
   work.
4. **Board config:** add `BOARD_HAS_ST25R3916` + the SPI/IRQ pin defines to
   `board_m5stack_cardputer_adv.h`; make the NFC backend selectable (ST25R3916 vs
   PN532) at build or runtime. Keep the PN532 path for users on that hardware.

## Feasibility / risk

- The ST25R3916 fully supports everything the NFC stack needs (it's the reference
  chip) — so mfkey collection, magic Gen1a backdoor (7-bit `0x40`), and nested
  become the *intended* case, not a hack.
- Main risks: (a) confirming the M5Stack unit's bus (SPI vs I2C) + IRQ pinout;
  (b) the volume of STM32→ESP32 HAL substitution in the vendored driver; (c)
  interrupt latency / SPI throughput on ESP32 for the tight NFC timing (should be
  fine at ESP32-S3 clocks, but validate).
- This is a **multi-day port**, not a small feature — hence the dedicated handoff.

## Testing

- Bring-up: `furi_hal_nfc_init` succeeds, oscillator starts (`OscOn`), chip ID
  register reads the ST25R3916 signature.
- Poller: read an ISO14443a UID; then a full MIFARE Classic read.
- Listener/emulate: emulate a card; verify Detect-Reader collects mfkey nonces
  (which then feed the already-built on-device recovery).
- Magic: Gen2 block-0 write; Gen1a backdoor via custom-parity.

## Relationship to shipped work

The **on-device mfkey recovery is already built + verified** (crapto1 core +
worker-thread UI, this session). It is chip-independent and will "just work" once
this ST25R3916 driver lands and Detect-Reader can collect nonces on the unit. The
magic-write phase (spec `2026-07-10-nfc-mfkey-magic-design.md`) also depends on
this driver's raw-framing.

## Plan

See `docs/superpowers/plans/2026-07-10-st25r3916-driver.md`.
