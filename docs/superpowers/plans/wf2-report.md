# WF2 — WiFi WPS Recon — verification report

**Date:** 2026-08-13
**Branch:** `feat/redteam-gapfill` (PRIVATE, unpushed). Program item WF2 (`redteam-gapfill-roadmap.md`).

## What shipped
A PRIVATE-only passive **WPS Scan** in the WiFi app: identify which nearby APs advertise WPS and their posture. Rescoped from Pixie-Dust (infeasible — ESP-IDF's managed stack exposes no raw EAP-WSC).

| Layer | Files | Commit |
|-------|-------|--------|
| WPS-IE parser (pure, ASAN-tested) | `helpers/wlan_wps_parse.{c,h}` | 3e96bac |
| WPS Scan scene + menu wiring | `scenes/scene_wps_scan.c`, `scene_config.h`, `scene_main.c` | 4ac5790 |

- **Parser** walks tagged IEs → the WPS vendor IE (OUI 00:50:F2 type 0x04) → WPS attributes (version, state, AP-setup-locked, selected-registrar, config-methods). Fully bounds-hardened: every IE/attribute length validated before read; position arithmetic in `uint32_t` (no `uint16_t` wraparound on adversarial input); non-WPS vendor IEs (e.g. WMM type 0x02) not misparsed.
- **Scene** clones `scene_station_scanner.c`'s promiscuous-cb → mux-guarded SPSC ring → Tick-drain → channel-hop → on_exit-demote. Captures beacons/probe-responses, slices the IE region (24 hdr + 12 fixed = offset 36, `len>=36` guarded), parses, and upserts a BSSID-keyed AP list (cap 32) showing WPS present/locked/config-methods.
- **Passive only** — no TX / injection / association (verified: none of the TX HAL surface is called).
- **Gated `#ifndef PUBLIC_BUILD`** (wlan_app ships public; WF2 does not), mirroring WF1.

## Build gate — all fresh-log verified
| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 18:37:00) |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS` (log 18:45:54) |

## Public exclusion proof
GNU `nm` on public `furi_esp32.elf` (26,531 symbols; a known wlan public symbol present). **Precise** WF2 symbol scan:
```
wlan_wps_parse | wlan_app_scene_wps_scan | WlanAppSceneWpsScan
  count in public ELF: 0
```
- Sanity: `wlan_wps_parse_ies` **is** present in the PRIVATE build (1) and **absent** from public (0) — the feature is gated in, not merely unreachable.
- **Disambiguation note:** a broad `wps_scan` substring grep also matches `scan_fill_wps_scan_ie`, which is an **ESP-IDF WiFi-library internal** (present in BOTH public and private builds, unrelated to WF2) — NOT a leaked WF2 symbol. Precise patterns (`wlan_wps_parse`, `wlan_app_scene_wps_scan`) confirm 0 WF2 symbols in public.

## Host tests — ALL PASS (`-Wall -Wextra -fsanitize=address`, WSL gcc)
`test_wlan_wps_parse` (5 cases): present+unlocked+PBC, present+locked+PIN, no-WPS, non-WPS vendor IE (no false positive), truncated/over-claiming-length (full prefix sweep, no ASAN report → no OOB).

## Review outcomes (per-task, all Approved)
- **Task 1:** parser bounds-safety hand-traced end-to-end (holds); `uint32_t` arithmetic defeats wraparound; no false positive; no findings.
- **Task 2:** gating structurally verified against the scene-table x-macro (scene absent in public, indices 0–16 unchanged); passive-only confirmed; IE-slice underflow-safe; concurrency an exact station_scanner clone. 3 informational Minors (inherited non-volatile ring read; parser no early-break on duplicate WPS IE; 320B frame-copy cap = documented cosmetic) — all deferred.

## Honest limits (surfaced in-UI / docs)
- **Recon only** — reports what APs advertise; the actual WPS-PIN / Pixie-Dust attack is NOT performed on-device (the ESP-IDF stack can't do the raw EAP-WSC exchange). Pursue off-device (laptop + reaver/bully) using this intel.
- **Passive sniff is lossy** — sees an AP only when it beacons/responds on the dwell channel.
- **"Locked" is a live signal** — an AP advertising AP-Setup-Locked is currently defending WPS-PIN; reported truthfully.
- **320B frame-copy cap** — an unusually IE-heavy beacon could be truncated before its WPS IE (cosmetic; retried on the next beacon).

## HW status — DEFERRED
Host-logic + build-gate + symbol-exclusion verified. Not exercised against live WPS APs. Most home routers ship WPS-on — good on-hardware test targets.
