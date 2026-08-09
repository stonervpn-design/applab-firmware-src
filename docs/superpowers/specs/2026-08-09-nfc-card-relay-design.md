# NFC Access-Card Relay — Design (2026-08-09)

PRIVATE-only, authorized research. Two-device ISO14443-4 (APDU) relay over ESP-NOW
to extend the range of a 13.56 MHz proximity access card — the NFC twin of the
BLE PKE relay ([[ble-pke-relay]]). Test only against the owner's own cards/readers.
Design/spec only; no code yet.

## Goal

Make a target reader/door believe the owner's access card is present when it is
actually far away, by relaying the ISO14443 APDU exchange between two ESP32-S3 +
ST25R3916 devices: one emulating the card to the reader, one reading the real
card, bridged by ESP-NOW.

## Feasibility (what's already here)

- **ISO14443-4A APDU layer, both roles**: `iso14443_4a_poller.c` (read the real
  card) + `iso14443_4a_listener.c` (emulate a card to the reader). Plus the
  ISO14443-3A listener for anticollision/SELECT.
- **ST25R3916 HAL does card-emulation (target/listen) AND reader (poller)** —
  the HAL already switches between initiator and target framing (st25r3916_hal.c
  §"Listener / card emulation", passive-target IRQs, SELECT-done).
- **ESP-NOW transport** present (`applications/main/esp_now`, esp_now_packet.h) —
  ESP32-native, ~1–4 ms, the low-latency link the relay needs.
- **crapto1 / mf_classic** present (context; not used by an APDU relay).

## HARD CONSTRAINTS (these define scope)

1. **Frame-Delay-Time (FDT) is the NFC analog of BLE latency.** ISO14443
   readers expect the card's answer within a bounded time. A two-hop
   (reader ⇄ listener ⇄ ESP-NOW ⇄ poller ⇄ card) round-trip adds latency that
   can blow the FDT. **The enabler is ISO14443-4 WTX** (Waiting Time eXtension):
   the emulated card can request more time (S(WTX)) to cover the ESP-NOW
   round-trip. So the relay works at the **APDU (ISO-DEP) layer** where WTX
   exists — DESFire / JavaCard / ISO-DEP access badges. **Phase 0 must measure
   whether the target reader honors WTX for enough time.**
2. **MIFARE Classic is OUT.** Its crypto1 auth is ISO14443-3 with tight,
   WTX-less timing and stateful per-frame crypto — a two-hop relay cannot meet
   it. (Card-direct nested/hardnested key recovery already exists for Classic —
   see [[fzcard-nfc-static-parser-registry]] / the stock stack — a different
   attack.) Relay scope is ISO-DEP/APDU cards only.
3. **Two NFC front-ends required — a HARDWARE question for the owner.** Each
   relay end needs its own 13.56 MHz front-end. The known unit is the **M5 NFC2
   (ST25R3916) on the M5Stick's Grove port**; the Cardputer's NFC availability
   must be confirmed, or a second ST25R3916/PN532-class module sourced. Unlike
   the BLE relay (BLE is built into both ESP32s), NFC is an add-on here.

## Architecture

- **Reader-side** (at the door): ISO14443-3A + 4A **listener** — completes
  anticollision presenting the card's UID/ATQA/SAK (captured in Phase 1), then
  relays every APDU the reader sends to the card-side over ESP-NOW, issuing
  S(WTX) as needed while it waits for the answer.
- **Card-side** (at the card): ISO14443-4A **poller** — holds an ISO-DEP session
  with the real card and replays the relayed APDUs, streaming responses back.
- Data path: `reader ⇄ listener(BLE-free NFC) ⇄ ESP-NOW ⇄ poller ⇄ card`. The
  relay forwards opaque APDUs; it never needs the card's keys.
- ESP-NOW framing: `{seq, dir(cmd/rsp), len, apdu…}` chunked to the 250-byte MTU
  with seq/ack; the listener drives WTX from an ESP-NOW round-trip timer.

## Phased plan (each independently testable; STOP-and-measure gates)

- **Phase 0 — FDT/WTX + ESP-NOW latency harness (DO FIRST; the gate).** On the
  reader-side unit, emulate a trivial ISO-DEP card that answers each APDU after a
  forced delay, issuing S(WTX); measure the max delay the OWNER's target reader
  tolerates. Separately time an APDU→ESP-NOW→echo loop. The two numbers decide
  whether a real relay is viable against that reader, or bench-only. Report in-UI.
- **Phase 1 — capture + faithful card emulation.** Card-side poller reads the
  real card's UID/ATS/APDU profile; reader-side listener reproduces the
  anticollision + ATS so the reader opens an ISO-DEP session with the emulator.
  No relay yet — prove the reader accepts the emulated front.
- **Phase 2 — single-APDU relay.** Wire ESP-NOW forwarding for one
  command/response APDU with WTX; prove a real reader↔card APDU survives the
  two-hop path against an owner-controlled reader.
- **Phase 3 — full session relay.** Both ends, all APDUs, seq/ack + WTX pacing,
  dual-screen status/latency, authorized end-to-end test vs the owner's own
  badge + reader. Mirror to src_cardputer_adv only if that unit has NFC.

## Honest caveats (in-UI)

- ISO-DEP/APDU cards with WTX-tolerant readers only; MIFARE Classic and tight
  ISO14443-3 auth are OUT (hardware/timing ceiling, not a bug).
- Needs two NFC front-ends; single-antenna NFC can't sniff+relay itself.
- Modern access systems with challenge freshness + short WTX windows may defeat
  it even at the APDU layer — Phase 0's number is the real go/no-go.

## Open questions for the owner (before Phase 1)

1. Does the Cardputer (or a second unit) have an NFC front-end, or do we source a
   second ST25R3916/PN532 module?
2. The specific authorized reader/card to test — to confirm ISO-DEP + WTX
   tolerance (in scope) vs MIFARE Classic (out) before building past Phase 0.
3. Accept bench/short-range first, with the Phase 0 WTX+latency number deciding
   real-range viability?
