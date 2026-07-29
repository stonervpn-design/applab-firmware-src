# CAN Commander — Design

**Date:** 2026-07-11
**Status:** pending final confirmation

## Purpose

A CAN-bus commander/diagnostic app running on the Cardputer-ADV, using the
ESP32-S3's built-in **TWAI** controller through the **Grove** port + an external
CAN transceiver. Send arbitrary CAN frames, repeat them, monitor bus traffic, and
save frame presets. Intended for bench/diagnostic use on hardware the operator owns
or is authorized to test.

## Topology (from user)

Cardputer (this app) is the sender/receiver. A **real Flipper Zero on the same CAN
bus acts as a second node to ACK** frames — a lone CAN transmitter with no ACKing
peer errors and retransmits forever, so the Flipper closes the loop for bench work.
The app also supports no-ACK self-test mode for solo use.

## Hardware

- **Controller:** ESP32-S3 TWAI (`driver/twai.h`, legacy API — present in IDF 5.4.1).
- **Transceiver:** external CAN transceiver on the Grove port (assumed M5Stack CAN
  Unit / SN65HVD230-class). TWAI only needs TX/RX logic pins; the transceiver does
  CANH/CANL.
- **Pins:** Grove `G2 = GPIO2`, `G1 = GPIO1` (the QWIIC/Grove I2C pins). Default
  **TWAI_TX = GPIO2, TWAI_RX = GPIO1**. If no frames leave the bus, TX/RX are likely
  swapped for the specific transceiver — expose a Settings toggle to swap them.
- **Mutual exclusion:** these are the same pins as the built-in **PN532 NFC + RFID2**
  I2C bus. CAN and those NFC tools cannot run at once; the app installs the TWAI
  driver only while active and uninstalls it on exit so the I2C bus is freed.

## Features (v1 — "as much as possible")

1. **Send single frame** — ID (11-bit standard or 29-bit extended), DLC 0–8, up to 8
   data bytes; entered as hex. RTR flag optional.
2. **Periodic / repeat send** — fire a frame at a chosen interval (10/20/50/100/500 ms
   or custom), start/stop; runs in a worker task.
3. **Monitor / sniff** — live receive list: newest frames by ID + data + a per-ID hit
   count; passive read (works in Listen-only too).
4. **Presets** — save/load named frames to `/ext/can_commander/presets.csv` on SD;
   re-fire without re-typing.
5. **Settings** — bitrate (125/250/500k/1M, default **500k**); mode
   (**Normal** [needs ACK] / **No-ACK self-test** / **Listen-only**); TX/RX pin swap.

## Architecture (`applications/main/can_commander/`)

- **`can_frame.{c,h}`** — PURE, host-testable: parse a hex ID string + hex data-byte
  string into a validated frame struct (bounds: DLC ≤ 8, ID ≤ 0x7FF std / 0x1FFFFFFF
  ext, valid hex); format a frame back to a display string; presets CSV line
  encode/decode. No IDF deps. Host test `tests/test_can_frame.c`.
- **`can_hal.{c,h}`** — firmware TWAI wrapper: `can_hal_start(bitrate, mode, tx, rx)`
  (twai_driver_install + twai_start), `can_hal_stop` (twai_stop + twai_driver_uninstall),
  `can_hal_transmit(frame, timeout)`, `can_hal_receive(out, timeout)`, status/alerts.
  Maps our frame struct ↔ `twai_message_t`. Frees the driver on stop (releases pins).
- **App + scenes** (mirror lan_attack structure): `can_commander_app.{c,h}`,
  `_app_i.h`, `scenes/` = main (Send / Periodic / Monitor / Presets / Settings),
  send (ID+data input), periodic (interval + start/stop + live TX count), monitor
  (RX list view), presets (list/save/load), settings (bitrate/mode/pin-swap).
  Worker tasks (periodic TX, monitor RX) use the FuriThread join-before-free pattern
  from `lan_attack_recon.c`.
- **Registration:** add `"can_commander"` to `fam_config.py` APPS; `application.fam`
  MENUEXTERNAL, `fap_libs=["assets"]`, an `A_GPIO_14` icon. Category map:
  `can_commander -> GPIO / Hardware` (in `loader_menu_categories.c`).

## Testing

- **Host test** `tests/test_can_frame.c`: hex ID/data parsing (valid + rejects: bad
  hex, DLC>8, ID overflow), frame→string formatting, preset CSV round-trip.
- **Firmware build** green (EXIT=0, BUILD OK).
- **On-device:** flash; with the CAN Unit on Grove + the Flipper on the bus, send a
  frame and confirm the Flipper sees/ACKs it; monitor shows return traffic. (Manual.)

## Non-goals / YAGNI (v1)

No OBD-II PID/UDS decoding, no DBC signal decoding, no fuzzing/replay-attack automation,
no bus-off auto-recovery beyond a manual restart. Raw send + monitor + presets only.

## Risk

Moderate. TWAI wiring/pin-direction is the main unknown (mitigated by the TX/RX swap
toggle + self-test mode to verify TX without a bus). Pure frame codec is unit-tested;
driver is installed/uninstalled per session so it never fights the NFC I2C bus.
