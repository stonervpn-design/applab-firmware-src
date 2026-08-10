# BLE Attack app — design

**Date:** 2026-08-10
**Status:** Approved (brainstorm), implementing
**Scope:** New private-only `ble_attack` app for the ESP32 Flipper port (M5Stick S3 superset + Cardputer ADV mirror). Two attack modes: **HID Inject (BadBLE)** and **Conn Flood (DoS)**.

## Motivation

The port already has a deep BLE surface (`ble_spam` advertising spam, tracker detection, `ble_walk_hal` GATT client with connect/read/write, the reusable `ble_hid` peripheral component). Missing: an offensive BLE HID keystroke injector (wireless BadUSB) and a connection-exhaustion stress tool. Both are composed from existing primitives — no new NimBLE stack code.

## Non-goals

- No SD DuckyScript browser (built-in payload menu instead — YAGNI).
- No connection sniffing / MITM (single radio, infeasible).
- No public build exposure. Private-only, hard-gated.

## Architecture

New app `applications/main/ble_attack/`, standard Flipper scene-manager + view_dispatcher structure, mirroring `ble_spam` conventions.

```
ble_attack/
  application.fam            # registered #ifndef PUBLIC_BUILD
  ble_attack.h / .c          # app alloc/free, view_dispatcher, scene_manager
  ble_inject_engine.h / .c   # opcode payload -> layout -> ble_hid keypresses
  ble_inject_payloads.h / .c # curated built-in payloads (static opcode arrays)
  ble_flood_worker.h / .c    # FuriThread connect/disconnect churn over ble_walk_hal
  scenes/
    ble_attack_scene.[hc]    # scene enum + handlers table
    ble_attack_scene_warning.c   # one-time "authorized testing only" ack
    ble_attack_scene_menu.c      # HID Inject / Conn Flood / About
    ble_attack_scene_hid_config.c   # payload / OS / layout / name / pairing
    ble_attack_scene_hid_run.c      # advertise, wait, type, status
    ble_attack_scene_flood_scan.c   # scan + device list
    ble_attack_scene_flood_run.c    # flood + live counters
    ble_attack_scene_about.c
  views/                     # custom status view(s) if submenu/var-item-list insufficient
```

**Reused primitives (no changes to their public API where avoidable):**
- `components/ble_hid/ble_hid.h` — `ble_hid_alloc/free`, `ble_hid_start/stop_advertising`, `ble_hid_set_state_callback`, `ble_hid_kb_press/release/release_all`.
- `applications/main/ble_spam/ble_walk_hal.h` — `ble_walk_hal_start/stop`, `_start_scan/_stop_scan/_get_devices`, `_connect/_disconnect/_is_connected`.
- Layout `.kl` files at `/ext/badusb/assets/layouts/*.kl` (128 × uint16 ascii→HID map), same load mechanism as `ducky_script.c`.

## Mode 1 — HID Inject (BadBLE)

**Flow:** menu → `hid_config` (payload, target OS, layout, device name, pairing mode) → `hid_run` (advertise → wait connect → run payload → done). Status states surfaced in `hid_run`: Advertising / Connected / Typing / Done / Error.

**Engine (`ble_inject_engine`):** consumes an opcode list and a `uint16_t layout[128]` table, emits key events via `ble_hid_kb_press`/`ble_hid_kb_release`.

Opcodes (compact, enough for the built-ins):
- `OP_TYPE(str)` — per char: `key = (c<128)? layout[c] : NONE`, press+release; modifier bits from the layout high byte (same convention as ducky).
- `OP_KEY(mods, keycode)` — chord (e.g. GUI+R), press all, release all.
- `OP_DELAY(ms)`.
- `OP_ENTER` — convenience = `OP_KEY(0, ENTER)`.

**Target OS** selects which variant of a payload's opcode list is used (GUI key = Win/Cmd/Super differ; some payloads are Win-only).

**Built-in payloads (`ble_inject_payloads`)**, private:
1. Lock Screen (Win: GUI+L; Mac: Ctrl+Cmd+Q; Linux: GUI+L).
2. Open URL (preset) — Win: GUI+R → `run box` → type URL → ENTER; Mac: Cmd+Space → Safari → URL; Linux: best-effort.
3. Notepad message drop.
4. Win PowerShell reverse-shell one-liner (private; opcode-encoded; host/port are build-time constants documented in-app).
5. Wi-Fi profile dump (Win `netsh wlan export`).

**Limitation (documented in About + spec):** modern hosts require pairing acceptance (passkey / just-works). This is a wireless BadUSB against hosts that will pair, not a 0-click exploit.

## Mode 2 — Conn Flood (DoS)

**Flow:** menu → `flood_scan` (reuse `ble_walk_hal` scan, list devices by name/MAC/RSSI) → pick target → `flood_run` (intensity/duration, Start/Stop, live counters).

**Worker (`ble_flood_worker`):** a `FuriThread` that loops:
`ble_walk_hal_connect(target, &abort)` → on success immediately `ble_walk_hal_disconnect()`; optionally trigger a security/pairing request to churn the target's bonding path. Counts attempts / successes / failures, exposes a rate. Honors an abort flag set by Stop or scene exit. Continues past connect failures.

Framed as a stress-test; impact varies (some peripherals hang/reboot, many just refuse).

## BLE contention & lifecycle

NimBLE is a single host — only one mode runs at a time. HID mode = peripheral/advertising; flood = central/scan+connect. On entering a mode, take over BLE from `bt_srv` (same pattern `bad_usb`/`hid_app` use for BLE HID). On mode exit / app exit: stop advertising, disconnect, `ble_hid_free` / `ble_walk_hal_stop`, then `furi_hal_bt_reinit` and let `bt_srv` resume. Guard against double-alloc.

## Gating & safety

- `application.fam` app entry wrapped so it is compiled only `#ifndef PUBLIC_BUILD` (matches existing private-only apps). Never in the public build or webflasher.
- One-time `warning` scene acknowledgment before any attack mode.
- About scene restates authorized-use-only + the pairing limitation.

## Error handling

- Missing `/ext/badusb/assets/layouts` → fall back to compiled-in US ascii map.
- `ble_hid_alloc` failure or advertising failure → Error status, clean unwind.
- Flood connect timeouts → count as failure, keep looping.
- Scene exit mid-attack → abort flag, join worker, restore BT.

## Testing

- **Host unit test:** `ble_inject_engine` opcode → keycode resolution through a synthetic layout table (pure logic, no BLE). Mirrors prior host-tested encoders. Assert TYPE/KEY/modifier/layout mapping.
- **HW test (pending):** pair to a Win/Mac/Linux host and run each payload; run flood against a spare BLE peripheral and watch counters. Flagged pending in memory.
- **Build:** verify `src_m5stick_s3` superset; mirror to `src_cardputer_adv`; clean reconfigure (delete build dir) because CMake globs are cached when files are added. Confirm the app is absent from a `PUBLIC_BUILD` configure.

## Rollout

1. App shell + scenes + gating (builds, menus navigate).
2. `ble_inject_engine` + payloads + host test.
3. Wire HID run scene to engine + `ble_hid`.
4. `ble_flood_worker` + flood scenes.
5. Build both trees; mirror; update memory; HW test pending.
