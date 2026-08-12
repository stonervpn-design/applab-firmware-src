# nRF24 Red-Team Suite — verification report

**Date:** 2026-08-13
**Branch:** `feat/nrf24-redteam-suite` (off the KeeLoq HEAD, unpushed, PRIVATE-only)
**Scope shipped:** 3 of 4 planned features (Drone hijack deferred — see below).

## What shipped

| Layer | Files | Commit |
|-------|-------|--------|
| Capture/TX engine | `helpers/nrf24_capture.{c,h}`, `helpers/nrf24_capture_tx.c` | 732baa7, b104fef, 78bee26 |
| Sniffer + Replay scene | `scenes/scene_sniffer.c` | b104fef, 78bee26 |
| KeySniffer decode + scene | `helpers/nrf24_keysniffer.{c,h}`, `scenes/scene_keysniffer.c` | d635f36, f570b95 |
| Fuzz mutators + scene | `helpers/nrf24_fuzz.{c,h}`, `scenes/scene_fuzz.c` | a04cf2a, 3c5aa91 |
| Menu wiring + gating | `scenes/scene_menu.c`, `scenes/scene_config.h` | 124a1b9 |

All new scenes/helpers are **whole-file `#ifndef PUBLIC_BUILD`** (attack tooling —
matches the standing private-only rule; the MouseJack/jammer gating is the precedent).

## Build gate — all fresh-log verified (stale-log gotcha method)

Method for every build: delete the build dir → invoke the `.bat` by absolute path
via `cmd.exe` → confirm the log mtime is current, its terminal SENTINEL, and that
the changed `.obj` recompiled. Never trusted exit codes.

| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 00:05:13); `scene_menu.c.obj` recompiled 00:04:18 |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` (log 00:07:14); `scene_menu.c.obj` recompiled 00:06:17 |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS: no car material` (log 00:11:44) |

## Public exclusion proof (the load-bearing check)

Scanned the **public** ELF `build_m5stick_s3_public/furi_esp32.elf` and its `.map`
with GNU nm (26,531 total symbols — tool confirmed working; known public symbol
`nrf24_app_scene_menu_on_enter` present = grep sanity passes):

```
ATTACK symbols (nrf24_capture|nrf24_keysniffer|nrf24_fuzz|nrf24_tx_raw|
                scene_sniffer|scene_keysniffer|scene_fuzz|
                nrf24_app_scene_{sniffer,keysniffer,fuzz})
  ELF match count: 0
  MAP match count: 0
```

**Zero attack symbols in the public build.** The whole suite compiles to nothing
in public; the public menu shows only Spectrum / Jammer / MouseJacker.

## Host tests — all PASS (`-Wall -Wextra` clean, WSL gcc)

| Test | Cases | Result |
|------|-------|--------|
| `test_nrf24_capture` | 4 | ALL PASS (ring add/get/overflow-drops-oldest/reset) |
| `test_nrf24_keysniffer` | 11 | ALL PASS (HID→ASCII incl. shift, null/short/garbage guards) |
| `test_nrf24_fuzz` | 6 | ALL PASS (determinism, 4 mutators, seed-sensitivity, guards) |

No drone host test — that layer was not built (below).

## Deferred: Drone/RC hijack (Tasks 5–6) — NEEDS_CONTEXT

The Bayang / Syma X5C / Bitcraze-ESB / V2x2 packet builders were **not** implemented.
Each needs a real wire-format reference (Multiprotocol / Deviation source, or on-air
captures) to port + gate against known-good vectors. No such reference was provided,
and fabricating protocol formats/checksums would ship non-functional radio code that
falsely claims support. The suite's architecture leaves a one-file-per-family registry
slot for each; adding a family later is a self-contained task once a reference exists.

## Honest limits (surfaced in-UI where they bite the operator)

- **Promiscuous sniff is lossy** — the nRF24L01+ can't truly go promiscuous; the
  address-trick catches a fraction of packets and only while the target transmits.
  Strong for MouseJack-class devices, weak for arbitrary/encrypted.
- **KeySniffer decodes only UNENCRYPTED keyboards** (the KeySniffer CVE set); AES
  keyboards are immune. `KS_HID_REPORT_OFF=2` is a HW-refinable offset.
- **Fuzzer reports what it sent, not a crash verdict** — the on-screen header states
  "Effect is target-dependent -- reports what it sent, not a crash verdict." No
  on-device crash oracle exists.
- **`nrf24_tx_raw` re-inits the radio per call** (its documented contract), so the
  fuzzer's real throughput is bounded by init overhead, not the 20ms loop delay.

## HW status — DEFERRED (not proven on-air)

Everything above is host-logic + build-gate + symbol-exclusion verified. None of the
three scenes has been exercised against real nRF24 hardware or a live vulnerable
keyboard. Expect promiscuous-RX timing + per-channel dwell tuning on-device (per the
jammer overhaul: software-CS settle, dwell ≥ PLL settle). This is the remaining proof.

## Pre-existing note (unchanged by this branch)

The base `nrf24` app already gc's to 0 functional symbols in the public ELF, leaving
2 inert MouseJack strings (cosmetic, pre-existing). This branch adds no new public
symbols on top of that.
