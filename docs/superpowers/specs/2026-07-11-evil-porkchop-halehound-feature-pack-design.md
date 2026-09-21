# Evil-M5 / Porkchop / Halehound Feature Pack — Design

**Date:** 2026-07-11
**Target:** FZ-Cardputer ADV port (`Flipper-Zero-meets-M5Stack-Cardputer-1.15`), ESP32-S3, no PSRAM
**Status:** Approved design, pending implementation plan

## Goal

Port features the port does **not** already have, drawn from three Cardputer/ESP32
firmwares. Split into two parts.

**Part A — Recon / utility (6):**
- **Porkchop** — WiFi Spectrum Waterfall ("Hog on the Spectrum"), Charging screen
- **Evil-M5Project** — Standalone Karma Attack, Attack Detector, Probe/PNL Sniffer
- **HaleHound** — Unified Loot Browser

**Part B — Red-team (7):**
- **Evil-M5Project** — Reverse-TCP C2 implant
- **HaleHound** — MouseJack HID injection (NRF24), Auth Flood, Evil-Twin auto-combo,
  BLE GATT recon, UART serial monitor, Station Scanner

Explicitly **out of scope** (already present in the port): deauth, client-sniff+deauth,
beacon/SSID spam, EAPOL/PMKID/handshake capture, Evil Portal/Evil Twin, host/port scan,
MITM, live creds, BLE spam + tracker detector, ESP-NOW, SubGHz, NRF24 core. On-device
wardriving is out of scope (covered by the existing offline BruceJS analyzer). HaleHound's
chipset-specific CVE modules (WhisperPair / CVE-2025-36911 Fast Pair, Airoha RACE /
CVE-2025-20700) are out of scope — they target vulnerable peripheral chipsets that don't
exist on the Cardputer radio, so there is nothing to port.

All Part B tools are dual-use offensive-security primitives implemented for **authorized
testing on the operator's own hardware/networks**, consistent with the rest of this port.

## Existing architecture leveraged

- **Scenes** are macro-registered via `applications/main/wlan_app/scenes/scene_config.h`
  (`ADD_SCENE(prefix, name, id)`); handlers auto-declared in `scenes.h`.
- **Views** register in the `WlanAppView` enum in `wlan_app.h`; custom canvas views
  (`wlan_deauther_view.c`, `wlan_pmkid_view.c`) use a `View` + draw callback — the model
  for the waterfall.
- **WiFi HAL** (`wlan_hal.h`): `wlan_hal_set_promiscuous(enable, cb)`,
  `wlan_hal_set_channel(ch)`, `wlan_hal_send_raw(data,len)` (802.11 TX ≤64B),
  `wlan_hal_beacon_spam_start/stop`, and Karma harvest primitives
  (`wlan_hal_evil_portal_karma_get_ssid_count/get_current`).
- **Loot store**: `wlan_loot.h` — captures land in `/ext/loot/<YYYYMMDD_HHMMSS>/` with a
  `manifest.csv` (`timestamp,type,target,detail`). `wlan_loot_record()` appends rows.
- **Power HAL**: `furi_hal_power` (used by `applications/debug/battery_test_app`) exposes
  voltage/charge/current for the Charging screen.
- **Build**: `cmd.exe + build_ci.bat`, `IDF_TOOLS_PATH=~/.espressif`; icons are
  checked-in C (no Pillow); SD = `E:` FAT32.

## Menu integration

`wlan_app` main menu (`scenes/scene_main.c`) gains **one** item under the separator:
**"Recon+ ▶"** (`MainIndexReconPlus`), routing to a new `WlanAppSceneReconMenu` submenu.
That submenu lists the new WiFi-side tools (Parts A + B):

1. Spectrum
2. Karma
3. Attack Detector
4. Probe Sniffer
5. Station Scanner
6. Auth Flood
7. Remote C2
8. Loot

The **Evil-Twin auto-combo** is reached from the existing **Evil Portal** menu
(`scene_evil_portal_menu.c`) as "Evil Twin (auto)", since it composes portal + deauth.

Cross-app additions (each a new scene in its host app, surfaced in that app's own menu):
- **MouseJack HID inject** → `applications/main/nrf24/`
- **BLE GATT recon** → `applications/main/ble_spam/`
- **UART serial monitor** → `applications/main/gpio/`

The Charging screen is a **separate main app** (`power_monitor`), registered on the
desktop via the loader menu categories (`components/loader/loader_menu_categories.*`).

---

## Feature 1 — Spectrum Waterfall (Porkchop)

**Files:** `scenes/scene_spectrum.c`, `views/wlan_spectrum_view.{c,h}`, scene enum entry,
view enum entry `WlanAppViewSpectrum`.

**Mechanism:** promiscuous RX with a fast channel-hop worker (channels 1–13, ~80 ms
dwell). Per channel, accumulate frames-seen and peak RSSI for the dwell window. Each full
sweep pushes one row into a circular waterfall buffer (13 columns × N rows). Draw:
- Top strip: live RSSI bar per channel for the current sweep.
- Body: scrolling waterfall, cell intensity = normalized(activity, peakRSSI) mapped to a
  dither/greyscale ramp on the 1-bit display.

**Controls:** Left/Right = lock to a channel vs. auto-hop; OK = pause/resume; a filter key
cycles **ALL / VULN / HIDDEN**. VULN/HIDDEN tags come from a background `wlan_hal_scan()`
run once on enter: mark channels hosting OPEN/WEP (`authmode`) or hidden-SSID APs; the
filter dims non-matching channels. (Porkchop's SOFT/PMF filter and IMU "dial mode" are
dropped — base Cardputer has no IMU, and PMF state is not exposed by the scan record.)

**Pure logic (host-tested):** waterfall aggregator — folding (channel, rssi, count)
samples into normalized cell intensities and advancing the ring buffer.

## Feature 2 — Karma Attack (Evil-M5 / HaleHound)

**Files:** `scenes/scene_karma.c` (reuses `wlan_sniffer_view` for the readout), HAL
additions `wlan_hal_karma_start()` / `wlan_hal_karma_stop()` /
`wlan_hal_karma_get_stats()` in `wlan_hal.{c,h}`.

**Mechanism:** a background HAL task that (a) runs promiscuous to harvest probe-request
SSIDs (the client PNL) into a bounded, de-duplicated set (cap ~32 SSIDs), and (b)
beacon-floods that harvested set (rotating SSID + randomized source MAC via
`wlan_hal_random_mac`, channel rotation) so a probing device sees its remembered network
advertised. Reuses the existing beacon-TX frame builder. Stats: harvested-SSID count, top
probed SSID, beacon frames sent.

**Loot:** on stop, record `type=karma`, `target=<top ssid>`, `detail=<n harvested>`.

**Safety framing:** standard authorized-pentest primitive; implemented plainly, no
gating theatre.

**Pure logic (host-tested):** probe-request SSID-IE extraction + bounded dedup set insert.

## Feature 3 — Attack Detector (Evil-M5, defensive)

**Files:** `scenes/scene_attack_detector.c`, `views/wlan_detector_view.{c,h}`, enum
entries.

**Mechanism:** promiscuous + channel-hop, classifying each mgmt frame:
- **Deauth/disassoc flood** — subtype `0x0C`/`0x0A`; count per rolling second, flag when
  rate exceeds a threshold; capture attacker (transmitter) MAC.
- **Pwnagotchi** — beacon whose SSID payload looks like the pwnagotchi JSON advertisement
  (`{"name":`, `pwnd_tot` markers) or the known pwnagotchi source-MAC OUI.
- **Evil twin** — same SSID string observed under a second BSSID within the session.

**View:** headline counters (deauths/sec, pwnagotchi count, evil-twin count) + a scrolling
alert list (`time  type  detail`). Alerts appended to loot (`type=alert`).

**Pure logic (host-tested):** frame classifier (given a raw 802.11 header → alert type)
and the same-SSID/different-BSSID evil-twin tracker.

## Feature 4 — Probe / PNL Sniffer (Evil-M5)

**Files:** `scenes/scene_probe_sniffer.c`, `views/wlan_probe_view.{c,h}`, enum entries.

**Mechanism:** promiscuous, filter to probe-request frames (type mgmt, subtype `0x04`),
extract the SSID IE and transmitter MAC. Maintain a bounded table
`client MAC → set of probed SSIDs` (LRU eviction). View: scrolling `MAC → SSID` lines with
a live unique-client / unique-SSID count. On exit, write `probes.csv`
(`mac,ssid,first_seen`) into the loot session and record a manifest row.

**Pure logic (host-tested):** probe-request parse + the bounded MAC→SSID table
(insert/dedup/evict). Shares the IE parser with Karma.

## Feature 5 — Unified Loot Browser (HaleHound)

**Files:** `scenes/scene_loot_browser.c` (+ a second scene for the entry list if needed),
reuses existing submenu + text-box/widget views.

**Mechanism:** three levels via the existing `Storage` API:
1. **Sessions** — list `/ext/loot/*` directories (newest first).
2. **Entries** — parse the selected session's `manifest.csv` into
   `time · type · target` rows; also list raw files not in the manifest.
3. **Preview** — text-box view of the selected file (PMKID/creds/CSV), size-capped read.

Actions: **Delete session** (recursive remove) and **Wipe all loot** (clear `/ext/loot`),
each behind a confirm dialog.

**Pure logic (host-tested):** manifest.csv row parser (handles empty target/detail,
separator neutralization consistent with `wlan_loot_record`).

## Feature 6 — Charging / Battery screen (Porkchop)

**Files:** new app `applications/main/power_monitor/` — `application.fam`,
`power_monitor.c`, `views/power_view.{c,h}`, checked-in icon C, loader-category entry.

**Mechanism:** single canvas view polling `furi_hal_power` (voltage, charge %, current,
charging flag) on a ~1 Hz tick. Displays: big % + voltage, charge state, a rolling
voltage-vs-time curve (ring buffer of samples), and a charge-time / runtime estimate
derived from the recent charge-rate slope.

**Pure logic (host-tested):** charge/runtime estimator (samples → minutes remaining) and
the curve ring-buffer downsampling.

---

# Part B — Red-team features

## Feature 7 — Reverse-TCP C2 implant (Evil-M5)

**Files:** `wlan_app/scenes/scene_c2.c`, `wlan_app/wlan_c2.{c,h}` (TCP client + command
loop), config scene reusing the text-input view; enum entries. Reuses socket helpers from
`lan_attack/net_socket.c`.

**Mechanism:** operator sets `host:port` (text input, persisted). The implant opens an
outbound TCP connection to that listener (firewall-friendly reverse channel) and runs a
line-based command loop on the wlan worker. Bounded command set, each mapped to an existing
HAL/scene action:

| Command | Action |
|---|---|
| `status` | free RAM, uptime, WiFi state, loot session |
| `scan` | `wlan_hal_scan()` → AP list back over socket |
| `sniff <ch>` / `stop` | promiscuous frame count |
| `deauth <bssid> <ch>` / `stop` | deauth via existing primitive |
| `portal <ssid>` / `stop` | start/stop evil portal, stream captured creds |
| `loot ls` / `loot get <file>` | list + stream files from `/ext/loot` |
| `exit` | close channel, return to menu |

Non-blocking; the C2 view shows link state, bytes in/out, last command. Auto-reconnect with
backoff while the scene is active. No inbound listener (reverse-only) → no open port on the
implant. A matching minimal Python listener snippet ships in the app's docs comment (not a
build artifact).

**Pure logic (host-tested):** the command parser/dispatcher (line → verb+args → bounded
enum), independent of sockets.

## Feature 8 — MouseJack HID injection (HaleHound, NRF24)

**Files:** `nrf24/scenes/scene_mousejack.c` (+ a scan scene if the app pattern needs it),
`nrf24/nrf24_mousejack.{c,h}`; enum + menu entry in the nrf24 app.

**Mechanism:** classic Bastille MouseJack. (1) Promiscuous NRF24 sweep to discover dongle
addresses (Nordic/Logitech Unifying), building an address list with channel/rate. (2)
Select a target, then inject an unencrypted keystroke stream — the payload is a Ducky-style
script reusing the existing `bad_usb` HID keycode tables. Builds Unifying/Nordic ESB frames
with correct payload + checksum and paces keystroke/HID reports. View: discovered addresses,
selected target, inject progress.

**Pure logic (host-tested):** ESB/Unifying frame builder + checksum, and the Ducky-script →
HID-report sequence (shared keycode tables with `bad_usb`).

## Feature 9 — Auth Flood (HaleHound)

**Files:** `wlan_app/scenes/scene_auth_flood.c`, reuses `wlan_deauther_view` for the
readout; enum + Recon+ menu entry.

**Mechanism:** against the current target BSSID (or channel-mode BSSID), transmit a flood of
802.11 **authentication-request** frames, each with a random source MAC
(`wlan_hal_random_mac`) via `wlan_hal_send_raw`, rotating to exhaust the AP's association
table. Distinct from deauth (fills the AP rather than kicking clients). View: frames/sec,
elapsed, target. Reuses the deauther-style start/stop + tick model.

**Pure logic (host-tested):** auth-request frame builder (given src MAC + BSSID → valid
mgmt frame bytes, ≤64 B).

## Feature 10 — Evil-Twin auto-combo (HaleHound)

**Files:** `wlan_app/scenes/scene_evil_twin_combo.c`; entry from `scene_evil_portal_menu.c`.

**Mechanism:** orchestration over existing pieces. Pick a target AP (scan/target reuse) →
start Evil Portal cloning that SSID on the AP's channel → concurrently deauth the real BSSID
to herd clients onto the twin → capture creds through the existing portal path. A single
state machine drives portal + deauth start/stop together so exit cleanly stops both. No new
radio primitives — pure composition.

**Pure logic (host-tested):** the combo state machine (target → cloning → herding → capture
→ teardown) as a pure transition function.

## Feature 11 — BLE GATT recon (HaleHound "BLE Predator")

**Files:** `ble_spam/scenes/scene_gatt_recon.c`, `ble_spam/views/ble_gatt_view.{c,h}`;
enum + menu entry in the ble_spam app. Uses the NimBLE central role already linked by the
BLE stack.

**Mechanism:** scan nearby BLE advertisers → pick one → connect as central → enumerate GATT
services and characteristics (UUID, properties: read/write/notify) → display the tree; read
values from readable characteristics on demand. Read-only recon (no writes) to keep it a
reconnaissance tool. Reuses the existing BLE-name/UUID DB (`ble_uuid_db.c`) for friendly
labels. Respects the shared-radio rule: BLE-only scene (WiFi torn down on entry).

**Pure logic (host-tested):** GATT-tree model insert/dedup + UUID → friendly-name lookup.

## Feature 12 — UART serial monitor (HaleHound)

**Files:** `gpio/scenes/gpio_scene_uart_monitor.c`, `gpio/views/gpio_uart_view.{c,h}`;
enum + gpio menu entry. Reuses the existing GPIO pin-config plumbing.

**Mechanism:** configure RX/TX pins + baud (variable-item-list), open a UART on the ESP32-S3
via the firmware UART HAL, and passively display incoming bytes as a scrolling hex+ASCII
dump (ring buffer). Optional line TX from a text-input. A "log to SD" toggle appends the
capture into the loot session.

**Pure logic (host-tested):** hex+ASCII line formatter and the capture ring buffer
(wrap/overflow handling).

## Feature 13 — Station Scanner (HaleHound)

**Files:** `wlan_app/scenes/scene_station_scanner.c`, `wlan_app/views/wlan_stations_view.{c,h}`;
enum + Recon+ menu entry.

**Mechanism:** promiscuous + channel-hop; from data-frame addressing (the same
to_ds/from_ds logic in `scene_package_sniffer`) build an association map
`BSSID → {client MACs}` with per-client last-seen/RSSI. View: expandable AP → client list
with counts; OUI-labeled via `wlan_oui.c`. Export the map to the loot session on exit.

**Pure logic (host-tested):** the association-map builder (frame → (bssid, client) →
insert/dedup/evict), sharing the addressing helper with the sniffer.

---

## Testing strategy

Follows the port's existing host-test harness (`tests/`, pure-logic modules guarded so
`main()`/Furi entry points are excluded). Each feature isolates its parsing/aggregation/
math into a testable unit:

| Unit | Test file |
|---|---|
| Probe-request SSID-IE parser (shared: Karma, Probe, Detector) | `tests/test_wlan_probe_parse.c` |
| Bounded MAC→SSID table (insert/dedup/evict) | `tests/test_wlan_probe_table.c` |
| Attack-frame classifier + evil-twin tracker | `tests/test_wlan_attack_detect.c` |
| Waterfall aggregator | `tests/test_wlan_spectrum.c` |
| Manifest.csv parser | `tests/test_wlan_loot_manifest.c` |
| Charge/runtime estimator + curve ring buffer | `tests/test_power_estimate.c` |

On-device behavior (promiscuous capture, beacon TX, power readings) is verified manually
after flashing.

## Risk / constraints

- **No PSRAM** — all buffers bounded and static/small (waterfall ring, harvested-SSID set,
  MAC table LRU). No large heap allocations in RX callbacks.
- **Single shared radio** — WiFi promiscuous tools already tear down BT via
  `wlan_hal_release_bt()`; the new tools reuse that path. Only one WiFi tool runs at a time
  (scene-scoped start/stop, mirroring `scene_package_sniffer`).
- **RX callback discipline** — promiscuous callbacks stay minimal (count/copy into
  volatile state); parsing/aggregation happens on the scene tick, matching the existing
  sniffer.
- **802.11 TX ≤64 B** — Karma beacons must fit the `wlan_hal_send_raw` 64-byte limit
  (short SSIDs only; longer harvested SSIDs truncated/skipped).
```
