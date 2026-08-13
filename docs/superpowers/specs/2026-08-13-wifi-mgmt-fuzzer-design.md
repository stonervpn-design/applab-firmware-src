# WiFi 802.11 Management-Frame Fuzzer (WF1) — design spec

**Date:** 2026-08-13
**Status:** Proposed — awaiting user approval
**Scope:** PRIVATE build only (attack tooling). Public build must exclude it entirely.
**Tree:** `src_m5stick_s3`; new work in `applications/main/wlan_app/`.
**Program:** gap-fill item WF1 (see `docs/superpowers/redteam-gapfill-roadmap.md`).

## Goal
Add a generic **802.11 management-frame fuzzer** to the WiFi app: take a valid base
management frame (auth / assoc-req / probe-req / beacon / deauth), apply a chosen
mutation strategy, and transmit the malformed frames at a target AP (or broadcast)
in a rate-limited, abortable loop. Purpose: stress-test AP/client 802.11 parsers for
crashes/DoS. This is the one WiFi offensive primitive the app lacks — it already has
deauth, beacon-spam, karma, PMKID, handshake capture, evil-portal, MITM.

## Constraints (hard)
- **PRIVATE-only.** New scenes + fuzz core + menu entry all `#ifndef PUBLIC_BUILD`.
  Public build compiles them to nothing and the nm/map exclusion scan shows 0 new
  attack symbols. Precedent to mirror exactly: the subghz "Protocol Fuzzer" gating
  (`subghz_scene_config.h:47-51`, `subghz_scene_start.c:51-57,184-196`). Note:
  wlan_app currently has NO gated scenes — this introduces the first.
- **No new radio path.** Reuse `wlan_hal_send_raw()` (thread-safe queued TX,
  `wlan_hal.h:79`), `wlan_hal_set_channel()` (`wlan_hal.h:71`), and the existing
  frame builders in `pmkid_frames.c`. No direct `esp_wifi_80211_tx` unless a
  saturation mode is added later.
- **Honest framing (no oversell).** The run screen states: "Effect is
  target-dependent -- reports frames sent, not a crash verdict." No fabricated
  "vulnerable" result. Surface the real limits (below) in-UI.
- Build via PowerShell fresh-log method; host tests via WSL gcc.

## Architecture
```
  pmkid_frames.c (existing: auth/assoc builders)  +  new probe/beacon/deauth builders
                          │  base frame bytes
                          ▼
        helpers/wlan_mgmt_fuzz.{c,h}  (NEW, pure, host-tested, furi-free)
          wlan_mgmt_fuzz_next(base, base_len, mode, seed, step, out[256], &out_len)
                          │  mutated frame
                          ▼
   scenes/scene_mgmt_fuzzer_config.c → scene_mgmt_fuzzer_run.c  (NEW, gated)
          worker loop: build base → fuzz_next → wlan_hal_send_raw → repeat
          (clones scene_network_deauth.c's xTask + stop-flag + Tick-counter + on_exit)
```

## Components

### 1. Mutation core — `helpers/wlan_mgmt_fuzz.{c,h}` (NEW, pure/furi-free, host-tested)
`bool wlan_mgmt_fuzz_next(const uint8_t* base, uint16_t base_len, WlanFuzzMode mode,
 uint32_t seed, uint32_t step, uint8_t* out, uint16_t* out_len);`
- Deterministic `splitmix32(seed ^ step)` PRNG (same idiom as `nrf24_fuzz.c`).
- `WlanFuzzMode`:
  - `WMF_BITFLIP` — flip one bit at a step-derived position in the frame body.
  - `WMF_FIELD` — mutate a structural header field per step: frame-control subtype,
    duration, seqctl, and (for deauth/disassoc/auth) the reason/status/auth-alg code.
  - `WMF_IE_MANGLE` — corrupt a tagged information element: overstate/understate a
    tag length (classic parser-overflow trigger), truncate, or inject a zero-length
    tag. Walks the IE list starting at the frame's fixed-body offset for its type.
  - `WMF_LENSWEEP` — sweep the total frame length from a floor up to the cap.
  - `WMF_RANDOM` — fully random body bytes over a base header.
- Output clamped to `WLAN_FUZZ_MAX 256` and to `wlan_hal_send_raw`'s 128-byte
  effective ceiling flagged in the header doc (see limits). `out_len >= 24` (never
  emits a sub-header runt). Never writes past `out[256]`.

### 2. Base-frame builders — reuse + extend `wlan_mgmt_frames.{c,h}` (NEW thin wrapper)
- REUSE `pmkid_build_auth_open()` and `pmkid_build_assoc_req()` (`pmkid_frames.c`)
  for the auth + assoc-req seeds — no reimplementation.
- ADD `wlan_build_probe_req(out, cap, src, ssid)`, `wlan_build_beacon(out, cap,
  bssid, ssid, channel)`, `wlan_build_deauth(out, cap, bssid, sta, reason)` in the
  same hand-packed byte-offset style (deauth layout from `deauth_tmpl[26]`). These
  produce the valid base the fuzzer mutates. Host-tested for correct header offsets.

### 3. Scenes — `scene_mgmt_fuzzer_config.c` + `scene_mgmt_fuzzer_run.c` (NEW, gated)
- **Config:** pick base frame type (Auth/Assoc/Probe/Beacon/Deauth), pick mode
  (5 above). Target BSSID/channel read from `app->target_ap` (fallback
  `app->connected_ap`, final fallback broadcast `FF:FF:FF:FF:FF:FF` + current channel).
- **Run:** clone `scene_network_deauth.c` — dedicated `xTaskCreate` worker that, each
  iteration, rebuilds the base, calls `wlan_mgmt_fuzz_next`, TXes via
  `wlan_hal_send_raw`, at `WMF_TX_DELAY_MS` (~15 ms) with a `s_run` stop flag +
  `while(s_task) vTaskDelay(10)` join; live "frames sent / step / mode" counter
  polled on Tick; OK toggles start/stop; `on_exit` stops the worker. On-air indicator.
  Honest header text (above).
- Wire into `scene_main.c` submenu ("Mgmt Fuzzer") + dispatch, and
  `scenes/scene_config.h` (`MgmtFuzzerConfig`/`MgmtFuzzerRun`) — ALL `#ifndef PUBLIC_BUILD`.

## Data flow
target_ap → config scene (type+mode) → run worker: build_base(type) →
wlan_mgmt_fuzz_next(mode, step++) → wlan_hal_send_raw → counter++ → repeat until Back.

## Testing
- **Host (WSL gcc, pure, like nrf24_fuzz):** `tests/test_wlan_mgmt_fuzz.c` —
  determinism (same seed+step → same bytes), each mode's bounds (out_len in
  [24,256], never OOB), BITFLIP flips exactly one bit, FIELD touches the intended
  offset, IE_MANGLE keeps the frame within cap and only edits the IE region,
  LENSWEEP monotonic, RANDOM seed-sensitive + reproducible, NULL/short guards.
  Base builders: header byte-offset assertions (frame-control, addr1/2/3, type body).
- **Build gate:** private m5stick + cardputer green (fresh logs); public m5stick
  `SENTINEL_OK` + `NOCAR PASS`; nm/map scan of public ELF for `wlan_mgmt_fuzz` /
  `mgmt_fuzzer` / new builder symbols → 0.
- **HW (deferred, needs a target AP):** real on-air injection; some malformed frames
  may be rejected by the ESP32 driver before hitting the air (documented, not hidden).

## Honest limits (surfaced in-UI / header docs)
- **128-byte TX ceiling:** `wlan_hal_send_raw` copies ≤128 bytes, so long assoc/probe
  frames with large IEs are truncated. LENSWEEP/RANDOM cap at 128 for this path. (A
  future saturation mode using a dedicated xTask + direct `esp_wifi_80211_tx` could
  raise this.)
- **Sequence-control not freely fuzzable:** the STA TX path forces `en_sys_seq=true`
  (IDF 5.4), so the hardware overwrites seqctl — FIELD mode notes this.
- **Driver may drop malformed frames** before air; the tool reports what it *submitted*,
  not what transmitted, and never a crash verdict.
- **Effect is target-dependent** — no on-device crash oracle.

## Non-goals (YAGNI)
- No public exposure. No new radio driver. No response-parsing/crash-oracle in v1
  (RX-observe of AP replies is a possible v2). No data-frame fuzzing (mgmt only).
- No saturation/flood mode in v1 (the 128-byte queued path is the v1 vehicle).

## Success criteria
- Mgmt Fuzzer reachable from the WiFi menu (private build), 5 frame types × 5 modes,
  targets `target_ap`/broadcast, working abort + live counter + honest header.
- Pure core + base builders pass host unit tests.
- Private m5stick + cardputer green; public compiles to nothing + 0 new attack
  symbols in the public ELF/map — all fresh-verified.
- Real limits (128-byte cap, forced seqctl, driver-drop, no crash oracle) surfaced.
