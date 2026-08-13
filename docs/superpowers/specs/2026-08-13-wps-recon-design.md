# WiFi WPS Recon (WF2, rescoped) — design spec

**Date:** 2026-08-13
**Status:** Proposed — awaiting user approval
**Scope:** PRIVATE build only (gated, like WF1). `applications/main/wlan_app/`.
**Program:** gap-fill item WF2 (see `docs/superpowers/redteam-gapfill-roadmap.md`).

## Why rescoped (honest)
Full WPS **Pixie-Dust / PIN attack is INFEASIBLE on this WiFi stack**: it needs a
raw EAP-WSC registrar exchange (send M1, receive M2's public key + E-Hash1/2, offline-
crack the PIN). ESP-IDF's managed WiFi stack does not expose raw EAP-WSC frames, and
this tree has no WPS/WSC machinery (only EAPOL M1–M4 handshake parsing) and no
`esp_wps.h`. Rather than ship a stub that fakes an attack, WF2 delivers what IS
genuinely feasible and useful: **passive WPS reconnaissance** — identify which nearby
APs expose WPS and in what posture, so the operator can pursue the actual attack on a
capable platform (a laptop with `reaver`/`bully`). Recon, not an on-device attack.

## Goal
A "WPS Scan" screen in the WiFi app: promiscuously capture beacons / probe-responses,
parse the WPS Information Element, and list each AP with its WPS posture — **WPS
present, AP-setup-locked (the single most important field for attackability), config
methods (PBC / PIN / Label / Display), and WPS version**.

## Constraints (hard)
- **PRIVATE-only**, `#ifndef PUBLIC_BUILD` (same gating as WF1 — wlan_app ships in
  public builds; this feature must compile out; public nm-scan = 0 new symbols). WF1's
  `scene_mgmt_fuzzer_*` gating is the in-app precedent.
- **Passive only.** Promiscuous RX + IE parse. No frame injection, no association, no
  active WPS exchange. TX path is not touched.
- **No new radio path.** Reuse `wlan_hal_set_promiscuous()` + `wlan_hal_set_channel()`
  and the promiscuous-RX callback/ring idiom from `scene_station_scanner.c` /
  `scene_sniffer.c`.
- **Honest UI.** State that this is reconnaissance (what the AP advertises); the actual
  PIN/Pixie-Dust attack is off-device (this hardware can't do the EAP-WSC exchange).
  "AP-setup-locked = yes" means WPS-PIN is currently rate-limited/locked (a real
  attackability signal), reported truthfully.
- Fresh-log builds; host tests via WSL gcc for the pure parser.

## Architecture
```
  wlan_hal promiscuous RX (beacons + probe-responses)  [reuse]
        │  raw 802.11 mgmt frame
        ▼
  helpers/wlan_wps_parse.{c,h}  (NEW, pure, host-tested, furi-free)
     - walk the tagged IEs of a beacon/probe-resp body
     - find the WPS vendor IE (OUI 00:50:F2, OUI-type 0x04)
     - parse WPS attributes -> WlanWpsInfo {present, locked, config_methods, version, state}
        │  WlanWpsInfo
        ▼
  scenes/scene_wps_scan.c  (NEW, gated) — hop + capture + parse -> AP list
     rows: SSID | ch | WPS: <off | on | on(LOCKED)> | methods
```

## Components

### 1. WPS IE parser — `helpers/wlan_wps_parse.{c,h}` (NEW, pure/furi-free, host-tested)
- `typedef struct { bool present; bool locked; bool selected_registrar; uint16_t config_methods; uint8_t version; uint8_t wps_state; } WlanWpsInfo;`
- `bool wlan_wps_parse_ies(const uint8_t* ies, uint16_t ies_len, WlanWpsInfo* out);`
  Walks tagged IEs (`id,len,val…`), finds the vendor IE `0xDD` with OUI `00:50:F2` +
  type `0x04`, then walks WPS attributes (2-byte big-endian type, 2-byte big-endian
  length, value): Version `0x104A`, WPS State `0x1044`, AP-Setup-Locked `0x1057`,
  Selected-Registrar `0x1041`, Config-Methods `0x1008` (bitmask: PBC 0x0080, Label
  0x0004, Display 0x0008, Keypad 0x0100…). Returns false (present=false) if no WPS IE.
- **Bounds-hardened:** every IE/attribute length validated against the remaining
  buffer before reading; a truncated/malformed IE terminates the walk safely, never
  reads OOB. Pure → host-unit-tested with real WPS-IE byte vectors (present/locked/
  unlocked/truncated/no-WPS).
- A small helper `wlan_wps_body_offset()` or reuse of the sniffer's frame-parse to get
  the IE region start (beacon: fixed 12-byte body after the 24-byte header; probe-resp:
  same 12-byte fixed params, then IEs).

### 2. Scan scene — `scenes/scene_wps_scan.c` (NEW, gated)
- Clone `scene_station_scanner.c`'s promiscuous hop + mux-guarded ring + Tick-drain +
  on_exit demote pattern. On each captured beacon/probe-resp: extract the IE region,
  `wlan_wps_parse_ies`, and if `present`, upsert an AP row keyed by BSSID with
  {SSID, channel, locked, config_methods}.
- List view: `SSID  ch7  WPS:ON(LOCK)  PBC/PIN` style rows; a header line stating this
  is passive recon. Back exits, standard abort.
- Register `ADD_SCENE(wlan_app, wps_scan, WpsScan)` + a "WPS Scan" submenu item +
  dispatch in `scene_main.c` — all `#ifndef PUBLIC_BUILD` (mirror WF1).

## Data flow
promiscuous RX → beacon/probe-resp → IE region → wlan_wps_parse_ies → WlanWpsInfo →
BSSID-keyed AP list → Tick-refreshed rows.

## Testing
- **Host (WSL gcc, pure):** `tests/test_wlan_wps_parse.c` — real WPS-IE vectors:
  (a) WPS present + unlocked + PBC; (b) present + AP-setup-locked; (c) present +
  PIN/Label config methods + version 2.0; (d) no WPS IE → present=false; (e) truncated
  WPS IE / bad attribute length → safe (no OOB, present per whatever parsed); (f) a
  non-WPS vendor IE (different OUI) → not misparsed as WPS. Assert the parsed fields.
- **Build gate:** private m5stick + cardputer green (fresh logs); public m5stick
  `SENTINEL_OK` + `NOCAR PASS`; nm/map scan for `wlan_wps` / `wps_scan` symbols → 0.
- **HW (deferred, needs a WPS AP):** confirm real APs' WPS posture is read correctly;
  many home routers ship WPS-on — good test targets.

## Honest limits (surfaced in-UI / docs)
- **Recon only** — reports what APs advertise in beacons/probe-responses. The actual
  WPS-PIN / Pixie-Dust attack is NOT performed on-device (the ESP-IDF stack can't do the
  raw EAP-WSC exchange); pursue it on a laptop with reaver/bully using this intel.
- **Passive sniff is lossy** — sees an AP only when it beacons/responds on the dwell
  channel; a full sweep may miss brief transmitters (same limit as the other sniff
  scenes).
- **"Locked" is a live signal** — an AP advertising AP-Setup-Locked is currently
  refusing WPS-PIN (rate-limited/defended); reported truthfully, not hidden.

## Non-goals (YAGNI)
- No on-device PIN/Pixie-Dust attack (infeasible — stated, not faked).
- No default-PIN database in v1 (user chose plain recon; can add later).
- No public exposure; no new radio/TX path; no association.

## Success criteria
- "WPS Scan" reachable from the WiFi menu (private build); lists nearby APs with WPS
  present/locked/config-methods/version, live-updating, with a working abort.
- The pure WPS-IE parser passes host unit tests against real byte vectors incl.
  truncated/malformed (no OOB) and non-WPS-vendor-IE (no false positive).
- Private m5stick + cardputer green; public compiles to nothing for WF2 + 0 new
  symbols — fresh-verified.
- Honest limits (recon-only, attack is off-device, lossy sniff, locked=defended)
  surfaced in-UI.
