# Flipper Share NFC — port to the ESP32 Flipper port — Design

**Date:** 2026-07-31
**Status:** Approved direction; Phase 1 starting.
**Target:** M5Stick S3 first (`NEW_v2/src_m5stick_s3`), then Cardputer ADV
(`src_cardputer_adv`). NFC hardware = **M5 Unit NFC (ST25R3916)** over Grove I2C.

## Goal

Port @lomalkin's **Flipper Share NFC** (`github.com/lomalkin/flipper-zero-apps`
/`flipper_share_nfc`) — direct file transfer between devices over the NFC antenna
(ISO14443-3A) — to this ESP32 port, so it can ship in the firmware and be listed
in the applab catalog.

## Why it's feasible (grounded in the source)

- Flipper Share's link layer (`nfc_transport.c/.h`) is a packet pipe over an
  ISO14443-3A **poller/listener pair**: the **sender emulates a card (listener)**,
  the **receiver is the reader (poller)**; each exchange carries ≤1 packet each way
  in a raw type-A frame `[hdr(1)][packet]`, CRC-A handled by the stack.
- The emulated card is **SAK 0x00 (no ISO-DEP/Type-4)** — plain type-A standard
  frames. No APDU/Type-4 layer needed.
- The poller **frame-wait-time is ~74 ms** (`NFC_TP_FWT_FC 1000000`), deliberately
  huge to "cover the listener's software response latency." The config comments
  reference "a stuck **ST25R3916** target automaton" — i.e. this transport was
  already written/tuned for a slow-software ST25R3916 listener. That same latency
  tolerance is what lets it survive the I2C link (vs the Flipper's SPI).
- The port's `furi_hal_nfc` **already exposes the needed APIs**: poller `tx/rx`,
  listener `tx/rx`, `iso14443a_listener_set_col_res_data`,
  `iso14443a_listener_tx_custom_parity`, listener `wait_event/enable_rx/sleep/idle`.

## The one hard gap

The port's **ST25R3916 driver is reader-only today.** `st25r3916_nfca.c`
implements poller REQA/anticollision/SELECT and **masks the target IRQs**
(`IRQ_MASK_TARGET = 0x00`). The `furi_hal_nfc` listener path that exists is the
**PN532 backend** (Bruce-style ISO-DEP NDEF emulation) — the user does not have a
PN532 unit. So the project's make-or-break work is adding **NFC-A listener /
passive-target mode to the ST25R3916 driver** and wiring it under the existing
`furi_hal_nfc` listener API. The chip supports target mode; the port just never
implemented it.

## Architecture / units of work

Each unit has one clear job and a testable boundary:

- **`st25r3916` target mode (driver):** configure the ST25R3916 as an NFC-A
  passive target — respond to REQA/WUPA→ATQA, anticollision→UID, SELECT→SAK
  (hardware auto-response where the chip supports it; FIFO/IRQ-driven otherwise),
  then `listener_rx` (receive a reader frame) / `listener_tx` (answer). Depends
  on: `st25r3916_i2c`, `st25r3916_reg`. Consumed by: `furi_hal_nfc` listener.
- **`furi_hal_nfc` listener backend wiring:** route the existing listener API to
  the ST25R3916 target functions when the ST25R3916 unit is the active backend.
- **`nfc_transport`:** port verbatim-as-possible onto the two backends' poller +
  listener frame APIs.
- **`nfc_share` engine + scenes + file I/O:** the app.

## Phases (hardware-gated)

1. **Phase 1 — ST25R3916 NFC-A listener (THE GATE).** Add target-mode emulation
   to the driver + wire the `furi_hal_nfc` listener API. **Deliverable:** an
   on-device test where a phone (or a second unit in poller mode) activates the
   emulated card and completes at least one type-A data exchange over the 200 kHz+
   I2C. **User validates on hardware. If timing can't hold, the project stops here
   and we say so.**
2. **Phase 2 — transport.** Port `nfc_transport.c`; two M5 devices exchange a
   test payload (poller↔listener loop).
3. **Phase 3 — app + file transfer.** Port `nfc_share.c` (chunking, md5, block
   bitmap/ARQ, ETA) + scenes + SD file I/O; move a real file M5↔M5.
4. **Phase 4 — ship.** Into both firmware trees (private; public where it fits) +
   applab catalog entry (`const APPS` in index.html) once Phase 1–3 pass on
   hardware.

## Explicit non-goals / caveats

- **M5↔M5 only** in practice — needs the **Unit NFC (ST25R3916) on both** ends.
  RFID2 (WS1850S) is reader-only and cannot participate.
- **Interop with a real Flipper Zero is a stretch goal, not a commitment** —
  matching its exact NFC timing over I2C is unproven.
- **Throughput will be well under the Flipper's 7 KB/s.**
- No change to the existing reader/poller NFC behavior — target mode is additive.

## Phase 1 acceptance test

On the M5Stick S3 with the Unit NFC attached: start listener/emulation with the
transport's card identity (UID 7B, ATQA {0x44,0x00}, SAK 0x00); an Android "NFC
Tools"/phone or a second unit in poller mode must (a) see/activate the card and
(b) exchange one non-trivial frame both directions without the target automaton
wedging. Log the activation + the round-trip. Green = proceed to Phase 2.
