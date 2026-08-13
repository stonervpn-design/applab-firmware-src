# CAN1 — CAN-Bus Fuzzer — verification report

**Date:** 2026-08-13
**Branch:** `feat/redteam-gapfill` (PRIVATE, unpushed). Program item CAN1 (`redteam-gapfill-roadmap.md`).

## What shipped
A CAN-bus fuzzer inside the CAN Commander app (`applications/main/can_commander/`):

| Layer | Files | Commit(s) |
|-------|-------|-----------|
| Mutation core (5 modes, pure/host-tested) | `helpers/can_fuzz.{c,h}` | c17996a, ee05443 |
| Fuzz scene + wiring + app fields | `scenes/can_commander_scene_fuzz.c`, `can_commander_scene_config.h`, `can_commander_scene_main.c`, `can_commander_custom_event.h`, `can_commander_app_i.h` | f7311b3, +volatile fix |

- **5 mutation modes:** data bit-flip, data-random, **ID-sweep** (ECU discovery / targeted flood), DLC-sweep, full-random. Deterministic (splitmix32). Every output is TWAI-valid (`dlc` clamped ≤8 both before data[] indexing and after; `id` masked to 11/29-bit per `extended`).
- **Base frame:** the app's current `tx_frame` (from Send/Presets) else a default (id 0x100, dlc 8, data 0..7).
- **TX:** via `can_hal_transmit` (no new driver path); clones `can_commander_scene_periodic.c`'s FuriThread worker + stop-flag + stepped-sleep abort (Stop checked ≤20 ms) + `can_hal_start`/`can_hal_stop` lifecycle.
- **Honest bus-state indicator:** the "Bus" row is real `can_hal_stats()` (tx_ok/tx_fail/bus_err/bus_off); `can_hal_recover()` fires once per bus-off episode. On-screen, verbatim: "Bus state=real TWAI (no-ACK may=no device). Not a crash verdict." No fabricated verdict.

## Build gate — all fresh-log verified
| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 16:13:09) |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` (log 16:14:49); scene obj 16:14:02 |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS` (log 16:16:58) |

## Public exclusion proof
Gating is **whole-app manifest exclusion** (`fam_config.py:272-274` removes `can_commander` from `APPS` when `_is_public`) — no in-source `#ifndef PUBLIC_BUILD`. GNU `nm` on public `furi_esp32.elf` (26,531 symbols — tool works; known public symbol present):
```
CAN symbols (can_fuzz|can_commander|can_hal|CanFuzz)
  ELF match count: 0
  MAP match count: 0
```
**Zero CAN symbols in the public build** — the entire CAN Commander app, fuzzer included, is absent from public firmware.

## Host tests — ALL PASS (`-Wall -Wextra` clean, WSL gcc)
`test_can_fuzz` (6 cases): determinism + TWAI-valid across all 5 modes, one-data-bit-flip, ID-sweep bounds (std+ext), DLC-sweep cycle, NULL guards, malformed-base (dlc=200) clamp.

## Review outcomes (per-task, all Approved)
- **Task 1:** masking/determinism/NULL/enum verified correct. Fixed 1 Important — moved the `dlc` clamp *before* data[] indexing (a malformed base with dlc>8 would otherwise overflow data[8]); added a dlc=200 guard test.
- **Task 2:** worker lifecycle (join/free/null on stop + on_exit, no UAF/double-free), frame-validity at the call site (base always dlc≤8 via can_frame_build), and honest bus-state UI all verified against the periodic precedent. Fixed 1 Minor — `fuzz_mode` made `volatile` (cross-thread read). Deferred 1 cosmetic (status-string width clip, snprintf-safe).

## Honest limits (surfaced in-UI / docs)
- **Needs an external CAN transceiver** on the Grove port (GPIO2 TX / GPIO1 RX); the ESP32 TWAI controller alone is not a bus driver.
- **DLC 0..8, classic-CAN timing only** — TWAI cannot emit DLC>8 / CAN-FD / out-of-spec frames; the fuzzer fuzzes the transmittable space and says so.
- **Bus-state ≠ crash verdict** — tx_fail/bus_err/bus_off are real controller counters; "no ACK" can simply mean no other node is present. Reported honestly, no fabricated verdict.
- **ID_SWEEP over 29-bit ext space is astronomically large** — sweeps sequentially from 0; the current ID is shown so the operator knows how far it has reached.

## HW status — DEFERRED
Host-logic + build-gate + symbol-exclusion verified. Not exercised on a real CAN bus / transceiver / ECU. The real TWAI bus-state indicator is the intended on-hardware proof point.
