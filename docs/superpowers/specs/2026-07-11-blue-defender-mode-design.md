# Blue-Team "Defender" Mode — Design Spec

**Date:** 2026-07-11
**Component:** `applications/main/defender` (new app) + shared locator/logger consumed by other specs
**Target hardware:** M5Stack Cardputer ADV (ESP32-S3, 8 MB flash, **NO PSRAM**, native
WiFi+BLE, CC1101 Sub-GHz, nRF24, PN532/ST25R3916 NFC, IR, GPIO, SD, 128×64 canvas +
color LCD, QWERTY)
**Status:** Draft design — authorized security-research tooling for the port owner

---

## 1. Goal

Add a **passive, receive-only blue-team subsystem** ("Defender") that detects and
locates local RF attacks across every radio the port already carries:

- **WiFi** (native promiscuous RX): deauth/disassoc flood, evil-twin/rogue-AP,
  beacon-flood, karma/Pineapple, and attacker-hardware fingerprinting.
- **BLE** (native scan): unwanted-tracker (AirTag/SmartTag/Tile) persistence alerting,
  and BLE-spam flood detection.
- **Sub-GHz** (CC1101 RSSI): jamming / RF-energy monitor with an LCD waterfall.
- **nRF24** (RPD sweep): 2.4 GHz occupancy monitor with a waterfall.
- **Drone Remote-ID** (WiFi beacon/NaN + BLE): decode FAA/EU Remote-ID, show
  ID/GPS/operator, RSSI-locate.
- **Integrated Defender Mode**: one screen running the WiFi + BLE detectors together
  with categorized alerts.
- **Purple-team fire-and-verify harnesses**: run a controlled attack (deauth /
  BLE-spam / Sub-GHz jam) in a lab, then confirm+measure with the matching detector.

This spec **owns** two shared building blocks that other specs (AirTag geiger, drone,
rogue-AP, jammer) consume:

- an **RSSI direction-finding "locator" UI component** (bar + hot/cold beep-rate), and
- an **SD anomaly/event logger** (timestamped CSV + bounded per-packet PCAP-to-SD).

Success criteria:

1. Each detector raises a debounced, categorized alert (banner + LED + beep) when its
   heuristic trips, and clears when the signal stops.
2. Every detector can hand its strongest offender to the shared locator and the user can
   home in on it by RSSI.
3. Every alert is appended to a CSV on SD with a timestamp; WiFi detectors can optionally
   dump the offending frames as a small bounded PCAP (current channel only).
4. On a no-PSRAM board the whole subsystem runs without ever buffering full frames in
   RAM — promiscuous callbacks copy only headers into small fixed ring buffers.
5. Enter/exit and mode switches leave every radio cleanly stopped (no stuck
   promiscuous mode, scan, speaker, or LED).

Non-goals are in §8.

---

## 2. Context — what already exists to reuse

**App framework** (see `applications/main/gpio/gpio_app.c`): the standard
`*_app_alloc/free` skeleton — `view_dispatcher_alloc`, `scene_manager_alloc`, custom /
back / tick callbacks (`view_dispatcher_set_tick_event_callback(..., 100)`), views added
by id, `scene_manager_next_scene(...)`, torn down in reverse in `_free`. Scenes are
declared with `ADD_SCENE(app, file, Name)` in `scenes/scene_config.h` and wired through
`scenes.c/.h` (see `ble_spam/scenes/scene_config.h`). Registration is in
`fam_config.py` `APPS` (this port does **not** auto-glob) plus an `application.fam`
`App(...)` manifest.

**WiFi promiscuous RX + raw TX — the `wifi_raw` HAL (DEPENDENCY, do NOT redefine here).**
The WiFi Attack Suite design owns the shared `wifi_raw` HAL: the promiscuous-RX callback
registration and raw 802.11 TX. Today those entry points live in
`applications/main/wlan_app/wlan_hal.{c,h}` as `wlan_hal_set_promiscuous(bool,
wifi_promiscuous_cb_t)`, `wlan_hal_set_channel(uint8_t)`, `wlan_hal_send_raw(const
uint8_t*, uint16_t)`, `wlan_hal_start/stop`, `wlan_hal_release_bt()`. Defender's WiFi
detectors consume `wifi_raw` for RX; the purple harnesses consume it for TX. **The exact
no-PSRAM RX pattern to copy** is `wlan_client_scanner.c`: a `static volatile` ring of tiny
records (`CsEvent { mac[6]; rssi }`, `CS_RING_SIZE 32`), the callback parses the 802.11
header, copies **only** the fields it needs, drops on full (`if(next == s_read_idx)
return;`), and a foreground `_drain()` consumes it. Frame-control/addr1-3/to_ds/from_ds
parsing is already worked out there and in `wlan_parse_helpers.h` (`wlan_ci_match`,
`wlan_ci_mem_find`).

**BLE scan + tracker classify** — `applications/main/ble_spam/ble_tracker_hal.{c,h}`:
passive GAP scan, `classify_advert()` → `TrackerKind` (AirTag Apple `0x004C` sub `0x12`,
Apple Nearby sub `0x10`, Apple PP sub `0x07`, Samsung SmartTag `0x0075`, Tile UUID
`0xFEED`/`0xFD84`), deduped `TrackerDevice { addr, addr_type, kind, name[24], rssi,
best_rssi, last_seen_ms, first_seen_ms }` up to `TRACKER_MAX_DEVICES` (40). A sweep mode
(`ble_tracker_hal_set_sweep`) already captures unknown adverts. The "following"
escalation + AD-name parser from the Tracker/Bug-Sweep spec (`tracker_follow.c`,
`ble_adv_extract_local_name`) is reused directly for the tracker detector.

**Existing RSSI "geiger" locator** — `ble_spam/views/tracker_geiger_view.{c,h}` +
`scene_tracker_geiger.c`: bar + beep proximity homing driven by `furi_hal_speaker_*`.
Defender generalizes this into the **shared locator** (§6.1) so drone/rogue-AP/jammer can
all reuse it, not just BLE trackers.

**Sub-GHz CC1101 RSSI** — `subghz/helpers/subghz_threshold_rssi.{c,h}`
(`subghz_threshold_get_rssi_data()` → `{ float rssi; bool is_above; }`) and
`subghz_txrx.{c,h}` for tuning + RSSI reads; `subghz_frequency_analyzer_worker.c` shows
the RSSI-polling worker loop. Board gating uses `_boards_without_subghz` in
`fam_config.py`.

**nRF24 spectrum** — `nrf24/nrf24_hw.h`: `nrf24_hw_acquire/release` (shared SPI bus),
`nrf24_hw_set_channel(0..125)`, and `nrf24_hw_listen_rpd(channel)` → RPD bit. Exactly the
primitive needed for a 126-channel occupancy sweep. Board gating uses
`_boards_without_nrf24`.

**PCAP + OUI/vendor on SD** — `wlan_pcap.{c,h}` already writes a
`LINKTYPE_IEEE802_11` (network=105) capture (`wlan_pcap_open/write_packet/close`) and
`wlan_oui.{c,h}` lazy-loads an OUI→vendor table from
`/ext/apps_data/wifi/mac-vendor.txt` (returns NULL on miss, per-lookup graceful). Both are
the templates for the shared logger and the fingerprint tables.

---

## 3. Design — per detector

Each detector is a **helper module** (`defender_<name>.{c,h}`) exposing a tiny lifecycle
(`start`, `stop`, `poll`/`drain`, `get_state`) plus a pure, host-testable heuristic
function, and a **scene + view** that renders it. All radio access goes through the
existing HALs; no detector owns a radio stack. All allocation is at scene-enter, freed at
scene-exit.

### 3.1 WiFi deauth / disassoc flood detector — `defender_deauth.{c,h}`

- **Signal source:** `wifi_raw` promiscuous RX, `WIFI_PKT_MGMT`. In the callback, match
  management subtype: deauth = frame type `0`, subtype `0x0C`; disassoc = subtype `0x0A`
  (`(fc & 0x00F0) >> 4`). Copy only `{ bssid[6], reason_code (u16), rssi, ts }` into a
  fixed ring (`DEAUTH_RING 48`). Never copy the frame body.
- **Heuristic** (`defender_deauth_eval`, pure): sliding 1-second window count of
  deauth+disassoc frames. Alert when `count/sec >= DEAUTH_RATE_ALERT` (default 20) — far
  above the handful a legit roam produces. Track per-BSSID subcount so the top offender
  BSSID is reported. Broadcast-target deauths (addr1 = `ff:ff:ff:ff:ff:ff`) weight higher
  (classic flood).
- **Alert:** category `WIFI/DEAUTH`, message `"Deauth flood ch<N> <rate>/s <bssid>"`.
- **UI:** live rate meter + top-offender list; OK sends the offender BSSID to the locator.
- **Channel:** hops 1–13 on a slow rotation (default 500 ms/ch) unless pinned; a flood on
  the current channel pins it (stops hopping) so the count is accurate.

### 3.2 Evil-twin / rogue-AP detector — `defender_eviltwin.{c,h}`

- **Signal source:** `wifi_raw` beacons (`WIFI_PKT_MGMT`, subtype `0x08`). Parse SSID
  (tag `0`) + BSSID + beacon body length + channel (DS-param tag `3`) + a cheap
  capability/security fingerprint (privacy bit, RSN tag `48` present). Copy only a compact
  `{ ssid_hash (u32 FNV of ssid bytes), bssid[6], beacon_len (u16), sec_flags (u8),
  channel, rssi }` record.
- **Baseline on SD (lazy-loaded, no RAM tables):** learned APs live in
  `/ext/apps_data/defender/ap_baseline.csv` (`ssid,bssid,beacon_len,sec_flags`). Loaded
  once at scene-enter into a small bounded array; "Learn network" appends the current
  AP.
- **Heuristic** (`defender_eviltwin_eval`, pure): flag when a beaconed SSID matches a
  learned SSID but **BSSID differs**, OR the `beacon_len` deviates from baseline by
  `> BEACON_LEN_DELTA` (default 16 bytes — captures a cloned SSID whose IE set differs),
  OR `sec_flags` downgraded (baseline had RSN, twin is open). Also flags a duplicate SSID
  on two BSSIDs simultaneously with a large RSSI gap (a nearby impostor).
- **Alert:** `WIFI/ROGUE_AP` — `"Evil twin '<ssid>' <bssid> Δlen=<n>"`.
- **UI:** list of suspicious SSIDs with both BSSIDs and the delta reason; OK → locator on
  the rogue BSSID.

### 3.3 Beacon-flood detector — `defender_beaconflood.{c,h}`

- **Signal source:** same beacon RX tap as §3.2 (shared parse; one promiscuous callback
  can feed several detectors — see §3.11 integrated tap).
- **Heuristic** (`defender_beaconflood_eval`, pure): count of **unique BSSIDs seen per
  second** (small Bloom-ish fixed hash set of the last N BSSIDs, cleared each window).
  Alert when `unique_bssid/sec >= BEACON_FLOOD_ALERT` (default 30). A real environment
  has a stable, small set; a `mdk4`/`beacon spam` run creates dozens of fresh randomized
  BSSIDs per second. Randomized-MAC ratio (locally-administered bit, addr2[0] & 0x02) is
  reported as a corroborating signal.
- **Alert:** `WIFI/BEACON_FLOOD` — `"Beacon flood <n> new APs/s"`.

### 3.4 Karma / Pineapple detector — `defender_karma.{c,h}`

- **Signal source:** `wifi_raw` probe-**responses** (subtype `0x05`) and beacons. Key
  insight: a karma AP answers *any* probed SSID from one radio. Maintain a fixed table
  `{ bssid[6], distinct_ssid_count, ssid_hash_set }` (bounded, ~16 APs × 16 SSID hashes).
- **Heuristic** (`defender_karma_eval`, pure): alert when one BSSID has responded for
  `>= KARMA_SSID_FANOUT` distinct SSIDs (default 5) within the window — a normal AP
  answers for its own 1–2 SSIDs. Combine with §3.5 fingerprint (Espressif/known-Pineapple
  OUI) to raise confidence.
- **Alert:** `WIFI/KARMA` — `"Karma AP <bssid> answering <n> SSIDs"`.

### 3.5 Attacker-hardware fingerprinting — `defender_fingerprint.{c,h}`

- **Signal source:** beacon/probe bodies already parsed by the shared tap; the OUI (first
  3 bytes of BSSID) and vendor-specific IEs.
- **Heuristics** (`defender_fp_classify`, pure, table-driven):
  - **Pwnagotchi**: beacon whose SSID/vendor-IE payload is JSON (`{"` prefix, `"name"`,
    `"pwnd_tot"` keys) — classic Pwnagotchi advertisement.
  - **Espressif OUI**: BSSID OUI in the Espressif ranges (many ESP32 attack tools spoof,
    but a fixed factory OUI + attack pattern is a tell).
  - **Pineapple heuristics**: known WiFi-Pineapple OUIs + the karma fanout of §3.4 +
    characteristic SSID sets.
- **Tables on SD (lazy):** `/ext/apps_data/defender/attacker_oui.csv`
  (`oui,label`) reusing the `wlan_oui` loader pattern (a second table instance). Missing
  file → fingerprinting degrades to "unknown", never crashes.
- **Alert:** `WIFI/HW_FINGERPRINT` — `"Pwnagotchi '<name>'"` / `"Espressif dev <oui>"` /
  `"Pineapple? <bssid>"`. Fingerprint is attached as a tag to any other WiFi alert for the
  same BSSID rather than always standing alone.

### 3.6 BLE unwanted-tracker detector — `defender_ble_tracker.{c,h}`

- **Signal source:** `ble_tracker_hal` scan (reused as-is) + the `tracker_follow.c`
  escalation logic already specced.
- **Heuristic:** a known-brand tracker (`TrackerKind` != Unknown) that persists across
  time with **rising or steady best_rssi** escalates `NEW→PRESENT→SUSPECT→FOLLOWING` via
  `tracker_follow_state(first_seen, last_seen, now)`. "Rising RSSI while dwelling" is the
  stalking signal. Reuses `FOLLOW_*_MS` thresholds.
- **Alert:** `BLE/TRACKER` — `"AirTag following (RSSI ↑)"`; debounced once per MAC until
  it goes stale (identical to the Tracker spec's alert bookkeeping).
- **UI:** shares the tracker list; OK → **shared locator** (this replaces the bespoke
  geiger for the integrated view, though the standalone BLE app keeps its own).

### 3.7 BLE-spam flood detector — `defender_ble_spam.{c,h}`

- **Signal source:** `ble_tracker_hal` in sweep mode (captures all adverts), reading
  manufacturer-data company IDs and service UUIDs from each advert.
- **Heuristic** (`defender_blespam_eval`, pure): per-second rate of adverts carrying
  spam-class payloads — Apple Continuity (`0x004C` with the pairing subtypes flood uses),
  Google/Android **Fast Pair** (`0xFE2C`), Microsoft **SwiftPair** (`0x0006`
  vendor-specific) — plus the **random-MAC flood rate** (distinct locally-administered
  BLE addresses/sec). Alert when `spam_payload/sec >= BLE_SPAM_ALERT` (default 25) or
  `new_random_mac/sec >= BLE_MAC_FLOOD_ALERT` (default 40). A `ble_spam` attack produces
  a torrent of fresh random MACs with these exact payloads.
- **Alert:** `BLE/SPAM` — `"BLE spam: Continuity <n>/s"` / `"FastPair flood"` /
  `"SwiftPair flood"`.

### 3.8 Sub-GHz jamming / RF-energy monitor — `defender_jammer.{c,h}`

- **Signal source:** CC1101 via `subghz_txrx` + `subghz_threshold_rssi`. Park on a chosen
  frequency (default 433.92 MHz; selectable), poll RSSI in a worker like
  `subghz_frequency_analyzer_worker.c`.
- **Heuristic** (`defender_jammer_eval`, pure): maintain a rolling noise-floor estimate
  (EMA of RSSI minima). **Jam = elevated RSSI sustained above `floor +
  JAM_DB_OVER` (default +18 dB) for `JAM_SUSTAIN_MS` (default 400 ms) WITH no valid
  decoded packet** in that span (a real transmitter would yield decodable bursts; a jammer
  yields continuous energy and zero valid packets). "No valid packet" is read from the
  Sub-GHz decoder's packet-complete signal (none arriving during high energy).
- **Alert:** `SUBGHZ/JAM` — `"RF jam <freq> +<db>dB"`.
- **UI:** LCD **waterfall** (`defender_waterfall_view`, §3.10) of RSSI over time + a
  current dB bar; OK → shared locator using live RSSI as the "signal" (walk toward the
  jammer).

### 3.9 nRF24 2.4 GHz spectrum / occupancy monitor — `defender_nrf_spectrum.{c,h}`

- **Signal source:** `nrf24_hw_listen_rpd(channel)` swept across channels 0–125 (126
  channels → 2400–2525 MHz). Wrap the whole sweep in one `nrf24_hw_acquire/release` per
  pass to respect the shared SPI bus.
- **Heuristic:** accumulate a per-channel occupancy count (RPD hits over K sweeps) into a
  `uint8_t occ[126]`. Flag channels with sustained high occupancy; overlay the WiFi
  channel map (WiFi ch1≈nRF 2412, etc.) so the user sees which WiFi channel is congested
  or jammed.
- **UI:** 126-column **waterfall** (shared with §3.10) scaled to the LCD width; brightest =
  busiest. This is a monitor, not an alerting detector (no debounced alert by default),
  though a "channel pegged" threshold can raise `RF24/OCCUPANCY`.

### 3.10 Waterfall view — `defender_waterfall_view.{c,h}` (shared by §3.8, §3.9)

A reusable scrolling-history view: caller pushes one column (an array of intensities +
optional peak marker) per tick; the view scrolls left and renders on the color LCD (and a
coarse mono fallback on the 128×64 canvas). Model: `{ uint8_t col[MAXBINS];
uint8_t nbins; int8_t peak_bin; char label[24]; }`. No radio knowledge — pure display.

### 3.11 Drone Remote-ID detector — `defender_remoteid.{c,h}` (+ `odid_parser.{c,h}`)

- **Signal source, two taps:**
  - **WiFi**: `wifi_raw` beacons + NaN (Neighbor Awareness Networking) action frames
    carrying the ASTM F3411 / ASD-STAN Remote-ID vendor IE (OUI `FA-0B-BC`, plus the
    Wi-Fi "Aware" service). Parsed from the same promiscuous beacon tap.
  - **BLE**: `ble_tracker_hal` sweep adverts with the ASTM Remote-ID service (Legacy:
    manufacturer data with the ODID app code; Extended-advertising Long-Range where the
    controller supports it).
- **Parser:** port the **opendroneid** message-pack logic into `odid_parser.c` — a pure
  decoder over the packed message structs (BasicID, Location, Operator/System). **No
  PSRAM needed**: it decodes a single ≤25-byte message set in place; we keep one
  `OdidUasData` per tracked drone in a small fixed table (`REMOTEID_MAX_DRONES` ~8), not a
  history. Strip opendroneid to the parse path only (no encoder, no C++).
- **Heuristic:** any well-formed Remote-ID frame → surface the drone. Persistence +
  rising RSSI raises a `DRONE/PRESENT` alert; a drone whose reported GPS is near the
  operator location is highlighted.
- **UI:** list of drones (UAS ID, type); detail shows ID / lat-lon-alt / operator
  ID+location / speed; OK → shared locator on that drone's RSSI ("walk to the pilot").

### 3.12 Integrated Defender Mode — `defender_integrated.{c,h}` + `scene_integrated.c`

One screen running **all WiFi + BLE detectors at once** off a **single shared promiscuous
tap** (one `wifi_raw` callback fans out to the deauth/eviltwin/beacon/karma/fingerprint/
remoteid parsers) plus the BLE scan (feeding tracker/spam/remoteid). Sub-GHz and nRF24 are
excluded from the always-on integrated view (they need exclusive radio time) and are
reached as separate scenes. Renders a rolling **categorized alert feed** (WIFI/BLE/DRONE
with severity color), a per-category live rate, and OK on any alert → shared locator on
its offender. WiFi channel hops slowly; a live alert pins the channel briefly to measure.

### 3.13 Purple-team fire-and-verify harnesses — `defender_purple.{c,h}` + `scene_purple.c`

Lab-only closed-loop tests. Each: **(1)** fire a controlled attack via the relevant TX
path, **(2)** run the matching detector, **(3)** report **detected? latency? measured
rate?**. Three harnesses:

- **Deauth**: TX a bounded deauth burst via the `wifi_raw` raw-TX path (owned by the WiFi
  Attack Suite) → confirm §3.1 fires and its measured rate matches the fired rate.
- **BLE-spam**: run the existing `ble_spam` TX for a fixed duration → confirm §3.7 fires.
- **Sub-GHz jam**: brief CC1101 carrier (or the nRF24 constant-carrier
  `nrf24_hw_jammer_start` for 2.4 GHz) → confirm §3.8/§3.9 flags energy.

Hard guardrails: a prominent "LAB USE" confirm screen, a **hard time cap** per fire
(e.g. ≤3 s), auto-stop TX before switching to the detector, and dependence on the same
random-MAC spoofing the TX HALs already do. These harnesses **depend on** the WiFi Attack
Suite's TX path and the CC1101/BLE TX — they do not add new TX code.

---

## 4. Alerts (common model)

All detectors emit through one small alert bus (`defender_alert.{c,h}`):

```c
typedef enum {
    DefenderCatWifiDeauth, DefenderCatWifiRogueAp, DefenderCatWifiBeaconFlood,
    DefenderCatWifiKarma,  DefenderCatWifiHwFp,    DefenderCatBleTracker,
    DefenderCatBleSpam,    DefenderCatSubghzJam,   DefenderCatRf24Occ,
    DefenderCatDrone,
} DefenderCategory;

typedef struct {
    DefenderCategory cat;
    uint8_t  severity;        // 0..2 (info/warn/critical)
    uint8_t  offender[6];     // BSSID / BLE MAC (for locator + logger); 0 if n/a
    int8_t   rssi;
    uint32_t ts_ms;
    char     msg[48];         // human-readable, pre-formatted
} DefenderAlert;

// Raise (debounced by (cat, offender) — see per-detector rules). Pushes to a small
// fixed ring for the UI feed AND to the SD logger.
void defender_alert_raise(const DefenderAlert* a);
// Feedback: banner string, RGB LED pulse, and one debounced beep (furi_hal_speaker_*),
// mirroring the tracker-scan alert path.
```

Debounce is per `(cat, offender)` with a re-arm on stale (same "already-alerted" fixed
set the Tracker spec uses).

---

## 5. Components — file layout

All under `applications/main/defender/`:

```
application.fam                         # App(appid="defender", entry_point="defender_app", ...)
icon.png
defender_app.c / .h / _i.h             # alloc/free skeleton (mirrors gpio_app.c)
defender_custom_event.h                # custom event enum

scenes/
  scene_config.h                       # ADD_SCENE(...) list
  scenes.c / scenes.h
  scene_start.c                        # main submenu: WiFi / BLE / SubGHz / nRF24 /
                                       #   Drone / Integrated / Purple / Locator test
  scene_wifi_menu.c                    # deauth / eviltwin / beacon / karma / fingerprint
  scene_deauth.c
  scene_eviltwin.c
  scene_beaconflood.c
  scene_karma.c
  scene_ble_menu.c
  scene_ble_tracker.c
  scene_ble_spam.c
  scene_jammer.c                       # SubGHz waterfall + jam alert
  scene_nrf_spectrum.c                 # nRF24 occupancy waterfall
  scene_remoteid.c
  scene_remoteid_detail.c
  scene_integrated.c                   # all WiFi+BLE detectors at once
  scene_purple.c                       # fire-and-verify (LAB)
  scene_locator.c                      # shared locator scene (§6.1)

views/
  defender_alertfeed_view.c / .h       # categorized rolling alert list
  defender_ratemeter_view.c / .h       # live per-detector rate + top offenders
  defender_waterfall_view.c / .h       # §3.10 shared waterfall (SubGHz + nRF24)
  defender_locator_view.c / .h         # §6.1 shared RSSI bar + hot/cold
  defender_remoteid_view.c / .h        # drone list + detail

# Detectors (helpers; each: lifecycle + pure eval)
defender_deauth.c / .h
defender_eviltwin.c / .h
defender_beaconflood.c / .h
defender_karma.c / .h
defender_fingerprint.c / .h
defender_ble_tracker.c / .h            # thin wrapper over ble_tracker_hal + tracker_follow
defender_ble_spam.c / .h
defender_jammer.c / .h
defender_nrf_spectrum.c / .h
defender_remoteid.c / .h
odid_parser.c / .h                     # ported opendroneid parse path (pure)
defender_integrated.c / .h             # shared WiFi tap fan-out
defender_purple.c / .h

# Shared, this-spec-owned (consumed by other specs)
defender_locator.c / .h                # §6.1 locator controller (RSSI→bar+beep)
defender_logger.c / .h                 # §6.2 CSV event log + bounded PCAP
defender_alert.c / .h                  # §4 alert bus + LED/beep/banner
defender_wifi_tap.c / .h               # single promiscuous cb → fixed rings, fan-out
```

**`fam_config.py` registration** — add `"defender"` to the `APPS` list (near `"wlan"` /
`"ble_spam"`, ~line 50). Because Sub-GHz and nRF24 features are board-gated, follow the
existing pattern: the app builds on all boards, but at runtime the SubGHz/nRF24 scenes
self-disable if the radio probe fails (Cardputer-ADV has both, so no exclusion entry
needed; do **not** add `defender` to `_boards_without_subghz/_nrf24` — those strip whole
apps).

**`application.fam`:**

```python
App(
    appid="defender",
    name="Defender",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="defender_app",
    stack_size=8 * 1024,          # WiFi promiscuous + SD logging → 8 KB like wlan
    fap_category="Tools",
    fap_libs=["assets"],
    fap_icon="icon.png",
    order=68,                     # right after wlan (67)
)
```

---

## 6. Shared components (owned here; interfaces other specs consume)

### 6.1 RSSI direction-finding locator — `defender_locator.{c,h}` + `defender_locator_view.{c,h}`

Generalizes `tracker_geiger_view` into a radio-agnostic homing UI. The **caller supplies
RSSI**; the locator owns the bar + hot/cold beep cadence + smoothing.

```c
typedef struct DefenderLocator DefenderLocator;

// smoothing: EMA over raw RSSI; beep cadence maps smoothed RSSI in [floor..ceil] to
// [slow..fast]. Reuses furi_hal_speaker_* exactly like scene_tracker_geiger.c.
DefenderLocator* defender_locator_alloc(void);
void defender_locator_free(DefenderLocator*);

// Bounds define the "cold" and "hot" ends of the bar/beep mapping (dBm).
void defender_locator_set_bounds(DefenderLocator*, int8_t rssi_floor, int8_t rssi_ceil);

// Push one fresh RSSI sample (from BLE scan, promiscuous rx_ctrl.rssi, CC1101 threshold
// rssi, whatever). stale=true → view greys out + mutes beep.
void defender_locator_push(DefenderLocator*, int8_t rssi, bool stale);

// A label shown above the bar ("AirTag", "Rogue AP <bssid>", "Drone", "Jammer 433.92").
void defender_locator_set_label(DefenderLocator*, const char* label);

View* defender_locator_get_view(DefenderLocator*);   // add to view_dispatcher
```

Scene contract: `scene_locator.c` is entered with `app->locator_target` populated
(label + bounds + a per-radio RSSI-poll callback). Any detector's OK handler fills that and
`scene_manager_next_scene(app->scene_manager, DefenderSceneLocator)`. Other specs
(AirTag, drone, rogue-AP, jammer) call the same three setters — they never re-implement a
geiger.

### 6.2 SD anomaly / event logger — `defender_logger.{c,h}`

```c
typedef struct DefenderLogger DefenderLogger;

// Opens /ext/apps_data/defender/events-YYYYMMDD-HHMMSS.csv, writes a header row.
// NULL on SD failure (logging becomes a no-op; detectors keep running).
DefenderLogger* defender_logger_open(void);
void defender_logger_close(DefenderLogger*);

// Appends one CSV row: ts_ms,category,severity,offender_mac,rssi,message
// Flushes on a bounded cadence (not per-row) to spare the SD. Called from
// defender_alert_raise().
void defender_logger_log_alert(DefenderLogger*, const DefenderAlert* a);

// --- Optional bounded per-packet PCAP (WiFi only, CURRENT channel only) ---
// Wraps wlan_pcap_open/write_packet/close. A SMALL RAM ring of pointer-free copies
// is NOT kept — instead each captured header is streamed straight to SD as it is
// dequeued from the promiscuous ring. Caller passes a cap; oldest rotates out on SD.
bool  defender_logger_pcap_begin(DefenderLogger*, const char* tag, uint32_t max_packets);
void  defender_logger_pcap_write(DefenderLogger*, const uint8_t* frame, uint16_t len,
                                 uint32_t ts_us);   // called from the drain loop, NOT the ISR/cb
void  defender_logger_pcap_end(DefenderLogger*);
```

**No-PSRAM rule baked into the interface:** `pcap_write` is called from the *foreground
drain*, never from the promiscuous callback, and streams one frame at a time — there is no
in-RAM frame buffer beyond the tiny promiscuous ring. `max_packets` bounds the file.

---

## 7. No-PSRAM discipline (explicit)

- **Promiscuous callbacks copy headers only** into small `static volatile` fixed rings
  (`DEAUTH_RING 48`, beacon-record ring, etc.), each holding compact structs (MAC + a few
  scalars), never the frame body. Drop-on-full, exactly like `wlan_client_scanner.c`.
- **One shared WiFi tap** (`defender_wifi_tap.c`) parses each frame once and fans the small
  record out to every WiFi detector, so the integrated mode does not multiply RAM.
- **Baseline / OUI / vendor / attacker-fingerprint tables live on SD**, lazy-loaded once
  per scene into bounded arrays (reuse the `wlan_oui` loader). Missing file → graceful
  degrade, never an allocation of the full dataset.
- **PCAP streams per-packet to SD** from the drain loop; no full-frame RAM buffering. The
  only RAM cost is the tiny promiscuous ring.
- **nRF24 sweep** stores a single `uint8_t occ[126]` (126 B), not sample history.
- **opendroneid** decodes one message set in place into one fixed `OdidUasData` per drone
  (~8 max); no message history, no dynamic allocation.
- **Fixed device caps**: `REMOTEID_MAX_DRONES 8`, karma `~16 APs × 16 SSID hashes`,
  eviltwin baseline bounded (e.g. 64 learned APs). All static or single scene-scoped
  `malloc`.
- Radios are **mutually exclusive on the shared antenna/bus**: entering a WiFi scene calls
  `wlan_hal_release_bt()`; BLE and WiFi are not run in the same scene except the integrated
  tap, which uses WiFi promiscuous + BLE scan the way the port already alternates them —
  verify coexistence on-target (see §9 open item).

---

## 8. Non-goals

- **No TX code of its own.** All active/TX behavior (deauth frames, BLE spam, carriers)
  belongs to the WiFi Attack Suite / `ble_spam` / Sub-GHz specs; the purple harnesses only
  *invoke* those and then measure.
- **No cloud / online lookups**, no GPS hardware (drone/tracker "location" is RSSI-relative
  + the drone's *self-reported* GPS only).
- **No full-frame capture/replay**, no deep packet inspection beyond the header fields each
  heuristic needs.
- **No cracking / deauth-to-capture chaining** — Defender is detect-and-locate only.
- **No settings UI in v1** — thresholds are tunable `#define`s (per §3). A settings scene
  is a later item.
- No simultaneous Sub-GHz + nRF24 + WiFi + BLE ("all radios") mode — they contend for the
  bus/antenna; integrated mode is WiFi+BLE only.

---

## 9. Testing

**Compile gate (must pass first):**
`MSYS_NO_PATHCONV=1 cmd.exe /c "...\build_ci.bat"` with `"defender"` added to `APPS`.
Model policy for implementation: **opus/haiku only** (Sonnet trips the cyber-safety gate).

**Host unit tests** (pure logic, `gcc`/WSL, like `pmkid_frames` / `tracker_follow`) — every
`*_eval` / parser is pure and deps-free:
- `defender_deauth_eval`: rate just below/above `DEAUTH_RATE_ALERT`; broadcast weighting;
  window slide correctness; per-BSSID top offender.
- `defender_eviltwin_eval`: same SSID + different BSSID → flag; beacon_len delta at the
  threshold boundary; security downgrade (RSN→open) → flag; identical AP → no flag.
- `defender_beaconflood_eval`: unique-BSSID/sec boundary; the fixed hash set doesn't
  false-count repeats within a window.
- `defender_karma_eval`: fanout boundary (4 SSIDs no-alert, 5 alert); table saturation.
- `defender_fp_classify`: Pwnagotchi JSON beacon → hit; Espressif OUI row → hit; unknown
  → "unknown"; malformed/short body → no over-read.
- `defender_blespam_eval`: Continuity/FastPair/SwiftPair payload rate + random-MAC flood
  boundaries.
- `defender_jammer_eval`: floor EMA tracking; sustained-over-floor + zero-packet → jam;
  brief spike with a decoded packet → no jam.
- `odid_parser`: a canonical ASTM Remote-ID BasicID+Location+Operator byte set → correct
  UAS ID / lat-lon-alt / operator; truncated/short buffer → returns false, no over-read;
  bad message-type → ignored.
- `defender_logger` CSV formatting (row shape, escaping) with a fake `File*`.

**On-hardware checklist (per detector):**
- **Deauth**: run the deauth purple harness (or an external tool) → banner + beep, rate
  meter matches, CSV row written, PCAP (if armed) contains the deauth frames; stops → alert
  clears.
- **Evil-twin**: "Learn" the home AP; stand up a same-SSID AP on a different BSSID (or the
  Attack Suite's rogue AP) → flagged with the Δlen/BSSID reason; locator homes on it.
- **Beacon-flood / Karma**: run beacon-spam / karma from a second device → both fire with
  plausible counts.
- **Fingerprint**: point a Pwnagotchi (or replay its beacon) → Pwnagotchi tag; an ESP32
  attack tool → Espressif tag.
- **BLE tracker**: leave an AirTag near the device 5 min with decreasing distance → escalates
  to FOLLOWING with rising RSSI; walk away → clears; locator beeps faster on approach.
- **BLE spam**: run `ble_spam` → BLE/SPAM fires; MAC-flood rate shown.
- **Jammer**: key a 433.92 carrier near the device → waterfall lights, +dB bar climbs,
  SUBGHZ/JAM fires; stop → clears; locator RSSI rises as you approach the source.
- **nRF24 occupancy**: run a 2.4 GHz load (WiFi download / BT stream) → the matching
  columns light; nRF24 constant-carrier → a single pegged column.
- **Drone Remote-ID**: with a Remote-ID-broadcasting drone (or a bench ODID broadcaster on
  WiFi/BLE) → drone appears, detail shows ID/GPS/operator; locator homes on RSSI.
- **Integrated mode**: two simultaneous attacks (deauth + BLE spam) both surface in the
  categorized feed with correct categories/severity.
- **Purple harnesses**: each fires within its time cap, auto-stops TX, then the detector
  reports detected + latency + measured rate; radios end cleanly stopped.
- **Lifecycle**: enter/exit every scene repeatedly → promiscuous off, scans stopped, no
  stuck speaker/LED, SD files closed, free RAM stable (no leak across 20 cycles).

---

## 10. Open items for planning

1. **WiFi+BLE coexistence in integrated mode** on this no-PSRAM S3: can native promiscuous
   RX and BLE GAP scan truly run concurrently, or must the integrated scene *time-slice*
   (e.g. 800 ms WiFi / 200 ms BLE) with `wlan_hal_release_bt()` off the table? Prototype
   early — this shapes §3.12.
2. **Shared promiscuous filter vs. per-detector filters.** One `esp_wifi_set_promiscuous_
   filter` mask must cover MGMT (beacons/probes/deauth/disassoc) + the NaN action frames
   for Remote-ID. Confirm one filter serves all WiFi detectors without dropping needed
   subtypes.
3. **opendroneid porting scope**: which upstream files/structs to vendor, license/attribution
   placement, and confirming the parser is pure C (strip any C++/encoder). Decide the
   `odid_parser` public surface before implementation.
4. **`wifi_raw` HAL surface finalization**: this spec assumes the WiFi Attack Suite exposes
   a stable RX-cb register + channel set + raw TX. If that spec renames/relocates the
   current `wlan_hal_*` promiscuous entry points, the Defender taps must follow. Coordinate
   the shared header name (`wifi_raw.h`?) before either app lands.
5. **Channel-hop vs. pin policy** shared across deauth/eviltwin/beacon/karma when several run
   in the integrated tap — one hop scheduler must serve all; define its pin-on-alert rule.
6. **Deauth "reason code" / broadcast weighting thresholds** and all `*_ALERT` defaults need
   a real-environment false-positive pass (busy café / apartment) before locking.
7. **PCAP rotation + SD wear**: cap size, number of rotating files, and flush cadence for
   `defender_logger` under a sustained flood.
8. **Sub-GHz "no valid packet" signal**: confirm the Sub-GHz decoder exposes a
   packet-complete hook the jammer eval can read; if not, spec a minimal RSSI-only jam
   heuristic fallback.
9. **Locator source callback lifetime**: the locator polls RSSI via a caller callback that
   touches a live radio — define ownership so leaving the locator scene can't outlive the
   detector that owns the radio.
```
