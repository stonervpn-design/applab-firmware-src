# Port Scope — Claude Buddy → Cardputer ADV ("mob")

> Feasibility scope for porting github.com/jxw1102/flipper-claude-buddy (v0.6, cat
> "Bluetooth") to this ESP32 static-link port. Companion to GAP_ANALYSIS_faps.md.
> Written 2026-07-14. Not yet started.

## What the app is
Flipper as a physical control surface + status display for Claude Code / Claude Desktop:
buttons approve/reject permission prompts + slash-commands, screen shows session status/
transcript/prompt, speaker+vibration feedback. Two transports:
- **Claude Code (USB/BLE)** via a companion Claude Code plugin + Python host bridge.
- **Claude Desktop (BLE)** direct, using Anthropic's Hardware Buddy protocol over
  Nordic UART Service (NUS) — no bridge.
SDK 1.4.1-rc / F7 API 87.1.

## Verdict: feasible, ~1 day for a working v1. NOT a Tier-A drop-in.

The port carries a real Flipper-compat shim (components/btshim, ble_profile/
extra_profiles/serial_profile.c, furi_hal/furi_hal_usb_cdc.c), so most of the 13 source
files compile against APIs that already exist.

## Component map (verified against the port tree)

| File(s) | Port status |
|---|---|
| claude_buddy.c (main) | portable |
| ui.c / ui.h (69 KB) | compiles vs port GUI reimpl; renders 128x64 into 240x135 color LCD (re-layout optional) |
| protocol.c, nus_charpack/protocol/state/stats/transcript.c | transport-agnostic logic - port as-is |
| serial.c (USB CDC) | SHIMMED (furi_hal_cdc_*, furi_hal_usb_*) - but ESP32-S3 CDC is the console/flash port; reconfigure may fight logging / re-wedge -> mark experimental |
| transport_bt.c (Flipper Serial BLE profile) | FULLY SHIMMED (bt_profile_start, ble_profile_serial_tx, bt_set_status_changed_callback, RECORD_BT) - the v1 BLE path |
| notifications.c | notification svc present; DROP vibro (Cardputer has no motor), speaker OK |
| app_settings.c | storage present |
| application.fam | EXTERNAL->MENUEXTERNAL, register appid in fam_config.py, icon clean |
| nus_profile.c / transport_nus.c (custom NUS GATT) | NOT shimmed - uses ble_gatt_service_add / ble_event_dispatcher_* (absent). Port does BLE via NimBLE ble_gatts_* directly. Needs rewrite -> DEFER to v2 |
| plugin/ (Python bridge, hooks, skills) | runs unchanged on the PC |

## Real work (3 items)
1. Manifest + registration + guard out the NUS path (#if around transport_nus/nus_profile)
   so it doesn't drag in absent ble_gatt_* symbols. Trivial.
2. Compile pass - shims present but not guaranteed signature-identical; expect a few
   build-fix iterations. ~1/2 day.
3. RAM tuning + on-device BLE test - the #1 risk. ~75-110 KB free heap, largest block
   ~31 KB, BLE-on already ~68 KB. Live GATT + transcript/state buffers + UI is tight ->
   shrink nus_state/nus_transcript buffers, watch for OOM reboots. ~1/2-1 day.

## Open question that sets v1 scope
Does the Python bridge's BLE (transport_bt.py) talk to Flipper's SERIAL-profile UUIDs or
to Nordic-UART UUIDs?
- Flipper serial profile -> v1 = transport_bt.c only, fully shimmed, NO GATT rewrite.
- Requires NUS UUIDs -> the NimBLE ble_gatts_* rewrite moves into v1 (+1 day).
Settle by reading transport_bt.py + the UUIDs in transport_bt.c.

## Plan
- v1: Flipper-serial-profile BLE + Python bridge -> Claude Code control from the Cardputer
  over BLE. Drop USB (or keep experimental), drop vibro, defer NUS.
- v2: rewrite nus_profile.c on NimBLE ble_gatts_* -> direct Claude Desktop pairing
  (Cardputer speaks Hardware Buddy protocol as a first-class buddy).
- v3: re-layout ui.c for the color screen.
Effort: v1 ~1 day; +v2 ~1 day; +v3 ~1/2-1 day.

---

## Session app-triage scorecard (lab.flipper.net picks, 2026-07-14)

| App | Fit for Cardputer ADV | Outcome |
|---|---|---|
| Blackjack / Dice D&D | Tier-A games | SHIPPED |
| PocketLab | pure UI/education | SHIPPED |
| Subhound | offline SubGhz .sub classifier (no live radio) | SHIPPED |
| Claude Buddy | BLE companion | SCOPED (this doc), ~1 day, feasible |
| Access Audit | multi-radio: 125kHz LFRFID + iCLASS/Picopass | REJECTED - no LF antenna, PN532 can't iCLASS -> belongs on real Flipper |
| Ghost ESP | ESP32 UART companion to external Ghost ESP fw | REJECTED - Cardputer IS an ESP32; wlan_app already covers it |

Fit rubric: pure UI/tools/games, offline parsers, CC1101 SubGhz, nRF24, IR, BadUSB, and
BLE-via-shim = portable. 125kHz LFRFID, iCLASS/Picopass, iButton, PSRAM-hungry, and
ESP32-companion apps = out. RAM caps the total compiled-in count to a curated few dozen.
