# ND1 — Bayang Drone Control (nRF24) — verification report

**Date:** 2026-08-14
**Branch:** `feat/redteam-gapfill` (PRIVATE, unpushed). Program item ND1 (`redteam-gapfill-roadmap.md`).

## ⚠️ UNVERIFIED — read this first
The Bayang wire format (packet layout, checksum, bind address/channel, RF channel-hop rule)
is **transcribed from public GPL Deviation / Multiprotocol sources from memory, with NO
reference capture and NO live drone to verify against.** Everything below proves the code
is *internally consistent, memory-safe, and correctly gated* — **NOT** that it controls a
real drone. A subtle bit-order / checksum / channel / bind-address error would pass every
check here and still not fly anything.

**The real proof is on-hardware: bind to an actual Bayang quad.** If it arms/flies, the
format is right and ND1 extends to Syma/ESB/V2x2. If not, capture + debug. The on-screen
banner says exactly this. Highest-risk transcription points (flagged `// UNVERIFIED:` in
code): the RF channel-hop derivation, the bind address/channel (`0x4B` / all-zero
placeholder), the axis byte offsets, the checksum `+0x55` seed, and the HEADLESS/RTH flag bits.

## What shipped
A PRIVATE-only "Drone (Bayang)" control feature in the nRF24 app:

| Layer | Files | Commit(s) |
|-------|-------|-----------|
| PHY: rate/CRC-aware TX setup | `nrf24_hw.{c,h}` (`nrf24_hw_tx_setup_ex`) | c0ae916 |
| Bayang packet builder (pure, ASAN-tested) | `helpers/drone_bayang.{c,h}` | a25bdb9, a7e2bc4 |
| Bind/control scene + menu wiring | `scenes/scene_drone_bayang.c`, `scene_config.h`, `scene_menu.c` | fc2c369, +bus-hold fix |

- **PHY (Task 1):** `nrf24_hw_tx_setup_ex(addr, addr_len, ch, len, data_rate, crc16)` — Bayang
  needs 1 Mbps + 16-bit CRC (`RF_SETUP=0x07`, `CONFIG=0x0E`), which the existing 2 Mbps/CRC-off
  `nrf24_hw_tx_setup` (left byte-for-byte unchanged) couldn't do. Gated.
- **Builder (Task 2):** pure, furi-free, host-tested under ASAN. Every write is a fixed
  byte index + per-byte mask (buffer-safe independent of clamping); axes clamped to 0..0x3FF;
  channels `%126` (≤125); checksum over the payload. Determinic.
- **Scene (Task 3):** worker clones scene_fuzz's stop-flag/join/Back-abort. BIND (~2s) then
  round-robin 4-channel CONTROL; buttons cycle+adjust the selected axis. Per-TX SPI
  acquire/release (bus free between frames — matters on shared-SPI T-Embed CC1101). Widget
  shows phase/axes/on-air + the verbatim UNVERIFIED banner. Radio released+deinit on every
  exit path.
- **Gated `#ifndef PUBLIC_BUILD`** throughout (nrf24 app ships public; ND1 does not).

## Build gate — all fresh-log verified
| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 00:04:59) |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS` (log 00:09:39) |

## Public exclusion proof
GNU `nm` on public `furi_esp32.elf` (26,531 symbols; known nrf24 public symbol present):
```
ND1 symbols (drone_bayang | bayang_ | tx_setup_ex)
  ELF match count: 0
  MAP match count: 0
Sanity: drone_bayang_build_control present in PRIVATE build = 1
```
**0 ND1 symbols in public** while the feature IS in the private build — gated in, not merely unreachable.

## Host tests — ALL PASS (`-Wall -Wextra -fsanitize=address`, WSL gcc)
`test_drone_bayang` (5 cases): checksum determinism + payload coverage, control packing +
checksum, bind≠control, channel-hop ≤125 across all 256 txid seeds, absurd-input axis clamp
(no ASAN OOB). **Internal consistency only — NOT real-frame correctness.**

## Review outcomes (per-task, all Approved)
- **Task 1:** register values verified vs nRF24 datasheet; existing `tx_setup` untouched. 2 by-design Minors.
- **Task 2:** memory safety traced (bounds independent of clamp); UNVERIFIED labeling thorough. 1 cosmetic Minor folded (comment wording).
- **Task 3:** worker lifecycle/radio-release/gating/banner verified. 1 Important folded — per-TX SPI bus hold instead of whole-session (shared-SPI LCD-starvation on T-Embed). 2 Minors deferred (unwired flag bits; labeled bind magic).

## Honest limits (surfaced in-UI / docs)
- **UNVERIFIED wire format** — transcribed from public docs, not tested vs a real drone/capture. May not control anything. (On-screen banner.)
- **One protocol (Bayang) only** — deliberately, to de-risk before building Syma/ESB/V2x2 blind.
- **nRF24-on-ESP32 TX timing is finicky** — the control loop may need on-device dwell/rate tuning.
- **Toy-grade** — Bayang-protocol hobby quads, not "any drone."

## HW status — DEFERRED (this is the real proof point)
Host-consistency + build-gate + symbol-exclusion verified. **Not tested against a real Bayang
receiver.** Next step is YOURS: bind to an actual Bayang quad. Fly → I extend to Syma/ESB/V2x2;
no fly → capture + debug the flagged transcription points.

### On-air debug checklist (if it doesn't bind), highest-likelihood first
1. **Control-phase address (most likely culprit — flagged by final review):** the worker sets
   the radio address once to the all-zero `bind_addr` and in CONTROL only changes channel, never
   switching to a **txid-derived data-phase address**. Real Bayang re-addresses after bind. If bind
   half-works but control doesn't, this is almost certainly why — add a `tx_setup_ex` with the
   data-phase address at the bind→control transition.
2. **RF channel-hop derivation** — the `%126` placeholder is not the real Bayang hop rule.
3. **Bind channel `0x4B` + all-zero bind address** — verify against `bayang.c`.
4. **Checksum `+0x55` seed** and **axis byte offsets** — verify against the source.
5. **PHY** (1 Mbps / CRC16) — confirm the clone chip actually locked 1 Mbps (no RF_SETUP read-back).
