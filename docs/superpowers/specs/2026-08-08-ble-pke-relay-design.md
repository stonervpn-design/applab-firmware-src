# BLE PKE Relay — Design (2026-08-08)

PRIVATE-only, authorized research. Two-device GATT-layer Bluetooth Low Energy
proximity-relay for passive-keyless-entry (PKE) systems, on the flashed ESP32-S3
devices. This is the design/spec — no code yet.

## Goal

Extend the effective range of a BLE PKE system so a target (car / smart lock)
believes the owner's phone/fob is in proximity when it is actually far away —
by relaying the BLE application traffic between two ESP32-S3 devices over a fast
side-channel. Test only against the owner's own vehicles/locks.

## Decisions locked (from brainstorming)

- **Target class:** general BLE PKE (protocol-agnostic), not Tesla-specific.
- **Relay ends:** M5Stick S3 (`70:04:1d:dc:5c:d0`) + Cardputer ADV
  (`d8:85:ac:a5:2e:44`), both ESP32-S3 with BLE (NimBLE) already in the port
  (`ble_spam`, Tesla scanner work) + ESP-NOW (`esp_now_app`).
- **Inter-device transport:** ESP-NOW (ESP32-native, ~1–4 ms, no AP).
- **Relay layer:** GATT / application layer (see constraint below).
- **Guarding:** private-only; ESP-NOW + BLE red-team tooling stays off public.

## HARD CONSTRAINTS (physics/hardware — no firmware fixes these)

1. **No link-layer relay on ESP32.** The BLE controller + NimBLE expose no raw
   link-layer PDU access or connection-following, so the NCC-Group-style
   <8 ms link-layer relay (which defeats latency checks and preserves end-to-end
   link encryption) is **impossible on this hardware**. Only a GATT/app-layer
   relay is buildable.
2. **The encryption boundary (defines efficacy).** A GATT relay forwards app
   messages across two *separate* BLE connections. It works ONLY when the PKE
   security is carried in the **app-layer payload over a non-link-encrypted
   connection** (blind-forward the crypto challenge/response — e.g. Tesla
   VCSEC-style protobuf, many smart locks). It CANNOT bridge a **bonded /
   link-encrypted** connection (car↔phone LTK from pairing) — two GATT
   connections can't share that key. In scope: app-layer-crypto, loose timing.
   Out of scope: link-encrypted PKE, latency-hardened (Tesla post-2022, UWB).
3. **2.4 GHz coexistence.** BLE and ESP-NOW (Wi-Fi) share one radio; concurrent
   use is software-coex time-sliced → added latency + jitter on both. This is
   the dominant, non-obvious risk for any target that enforces a round-trip
   timing bound. Must be measured on-device early (Phase 0), not assumed.

## Architecture

Two roles, each on one device:

- **Key-side** (near the real phone/fob): BLE **peripheral** that impersonates
  the target's identity (advertises the car/lock's name + GATT service/char
  UUIDs) so the phone connects and starts the challenge. Forwards every GATT
  write/read/notify it receives to the car-side over ESP-NOW.
- **Car-side** (near the car/lock): BLE **central** that connects to the real
  target and, presenting the phone's captured GATT operations, replays the
  key-side's forwarded messages and streams the target's responses back.

Data path: `phone ⇄ key-side(BLE) ⇄ ESP-NOW ⇄ car-side(BLE) ⇄ car`. The relay
forwards opaque app-layer bytes; it never needs to understand the crypto.

Shared framing over ESP-NOW: a tiny header `{conn_id, op(read/write/notify),
handle/uuid, len, seq}` + payload, chunked to the ESP-NOW 250-byte MTU, with
seq/ack so ordered GATT ops survive fragmentation.

## Phased plan (each phase independently testable; STOP-and-measure gates)

- **Phase 0 — Coexistence + latency harness (DO FIRST).** Bring up BLE + ESP-NOW
  concurrently on ONE device; measure the added round-trip latency/jitter of a
  BLE-GATT-op → ESP-NOW → back loop. This number decides the whole project's
  viability against any timed target. Report it honestly in-UI. If coex latency
  is prohibitive, the relay is bench-only — say so.
- **Phase 1 — GATT clone/enumerate.** Car-side connects to a target you own,
  enumerates services/characteristics, captures advertisement + identity.
  Key-side reproduces that GATT profile as a peripheral. No relay yet — just
  prove faithful impersonation (phone connects to key-side thinking it's the car).
- **Phase 2 — Single-link GATT proxy.** Wire the ESP-NOW forwarding for one
  characteristic write/notify round-trip; prove an app-layer challenge/response
  survives the two-hop path against a controlled target.
- **Phase 3 — Full 2-device relay.** Both ends, all GATT ops, seq/ack framing,
  a status/latency readout on each screen. Authorized end-to-end test vs the
  owner's own lock/vehicle.
- **Phase 4 — Robustness.** Reconnect handling, RPA / identity-rotation notes,
  target profile save/load, honest in-UI scope/latency banner.

## Honest caveats to carry in-UI

- Works only for app-layer-crypto, loosely-timed PKE (Phase 0 number gates this).
- Bonded/link-encrypted and latency-hardened targets are OUT — not a firmware
  gap, a hardware ceiling.
- Modern BLE identity privacy (resolvable private addresses / rotating MAC) can
  break static impersonation; note per-target.

## Open questions for the owner (before Phase 1)

1. The specific authorized target(s) to test — so we can confirm it's app-layer
   crypto (in scope) vs link-encrypted (out), before building past Phase 0.
2. Acceptable that this is bench/short-range-first, with the Phase 0 latency
   number deciding real-range viability?
