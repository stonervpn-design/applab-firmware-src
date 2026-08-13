# WF1 — 802.11 Management-Frame Fuzzer — verification report

**Date:** 2026-08-13
**Branch:** `feat/redteam-gapfill` (PRIVATE, unpushed). Program item WF1 (see `redteam-gapfill-roadmap.md`).

## What shipped
A PRIVATE-only 802.11 management-frame fuzzer in the WiFi app (`applications/main/wlan_app/`):

| Layer | Files | Commit(s) |
|-------|-------|-----------|
| Mutation core (5 modes, pure/host-tested) | `helpers/wlan_mgmt_fuzz.{c,h}` | d7b6f69, fd3659e |
| Base-frame builders (probe/beacon/deauth) | `helpers/wlan_mgmt_frames.{c,h}` | 44948be, 3df2b72 |
| Config + Run scenes + menu wiring | `scenes/scene_mgmt_fuzzer_{config,run}.c`, `scene_config.h`, `scene_main.c` | b006253, 6a52f03 |

- **5 frame types:** Auth / Assoc-Req / Probe-Req / Beacon / Deauth. Auth+Assoc reuse the existing `pmkid_frames.c` builders; probe/beacon/deauth are new.
- **5 mutation modes:** bit-flip, field (subtype/duration/seqctl/reason-status), IE-mangle (bad tag lengths), length-sweep, random. Deterministic (splitmix32), like the nRF24 fuzzer.
- **Target:** `app->target_ap` → `connected_ap` → broadcast + channel 1.
- **TX:** via `wlan_hal_send_raw` (no new radio path). Worker cloned from `scene_network_deauth.c` (xTaskCreate + stop-flag + blocking join + Tick-polled counter + OK-toggle + on_exit cleanup).
- All new code whole-file `#ifndef PUBLIC_BUILD`; menu entry/enum/dispatch + both `ADD_SCENE` gated. **First gated feature in wlan_app** (precedent: subghz Protocol Fuzzer).

## Build gate — all fresh-log verified
| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 13:45:10); run+config scene objs recompiled |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` (log 13:47:26); run scene obj 13:46:38 |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS` (log 13:49:27) |

## Public exclusion proof
GNU `nm` on public `build_m5stick_s3_public/furi_esp32.elf` (26,531 symbols — tool confirmed working; known public symbol `wlan_app_scene_main_on_enter` present):
```
WF1 attack symbols (wlan_mgmt_fuzz|wlan_build_probe|wlan_build_beacon|wlan_build_deauth|mgmt_fuzzer)
  ELF match count: 0
  MAP match count: 0
```
**Zero WF1 symbols in the public build.** Public WiFi menu is unchanged (no "Mgmt Fuzzer" entry).

## Host tests — ALL PASS (`-Wall -Wextra` clean, WSL gcc)
| Test | Cases | Result |
|------|-------|--------|
| `test_wlan_mgmt_fuzz` | 5 | ALL PASS (determinism, one-bit-flip, lensweep bounds, guards, IE-mangle bl==24 passthrough) |
| `test_wlan_mgmt_frames` | (deauth/probe/beacon offsets + cap-too-small + oversized-SSID clamp) | ALL PASS |

## Review outcomes (per-task, all Approved)
- **Task 1:** hand-traced all 5 modes' bounds — no OOB, `out_len` provably in [24,256]. Folded 3 minors (FIELD subtype clear-then-set, bl==24 test, default-fallback comment).
- **Task 2:** buffer-safety fully traced; SSID clamped via `strnlen(ssid,32)`. Folded 1 Important (beacon capability `0x01,0x04`→`0x11,0x00` to match its "ESS+Privacy" label) + probe/beacon cap tests.
- **Task 3:** critical buffer check clean (`out[WLAN_FUZZ_MAX]`=256); worker lifecycle matches deauth; enum 17 no-collision. Folded 1 Important (surface `Radio start failed`) + 128-byte TX clamp (LenSweep/Random >128 now transmit truncated instead of silently dropped).

## Honest limits (surfaced in-UI / code)
- **128-byte TX ceiling:** `wlan_hal_send_raw` caps at 128 bytes; mutated frames are truncated to 128 before TX (still valid fuzz input). A future saturation mode via a dedicated xTask + direct `esp_wifi_80211_tx` could raise this.
- **Sequence-control forced:** the STA TX path forces `en_sys_seq=true`, so hardware overwrites seqctl.
- **Driver may drop malformed frames** before air; the tool reports frames *submitted/queued*, never a crash verdict. On-screen: "Effect is target-dependent -- reports frames sent, not a crash verdict."
- **No crash oracle** — effect is target-dependent.

## HW status — DEFERRED
Host-logic + build-gate + symbol-exclusion verified. Not exercised on-air against a real AP/client. On-air injection + per-driver frame-acceptance is the remaining proof.

## Pre-existing note
`wlan_hal.h`'s `wlan_hal_send_raw` doc comment still says "64 bytes" (real ceiling 128) — stale, pre-existing, not introduced by WF1.
