# WiFi Attack Suite + shared `wifi_raw` HAL — Design Spec

**Date:** 2026-07-11
**Component:** new `applications/main/wifi_attack/` + shared `lib/wifi_raw/` (extracted from `applications/main/wlan_app`)
**Target hardware:** M5Stack Cardputer ADV — ESP32-S3, 8 MB flash, **no PSRAM**, native WiFi + BLE on-die (no companion ESP board needed), SD (FAT32), canvas UI, QWERTY.
**Framework:** reimplemented Flipper app framework on ESP-IDF v5.4.1 (scenes, view_dispatcher, scene_manager, submenu, variable_item_list, widget, text_input, popup, canvas).
**Model policy for later implementation:** opus/haiku only (Sonnet cyber-gate).
**Status:** Draft design — for review before an implementation plan.

---

## 1. Goal

Ship an authorized red-team WiFi attack suite for the native S3 radio, and — the
load-bearing architectural move — **extract the radio primitives that already
live inside `wlan_app` into a standalone, reusable `wifi_raw` HAL** that both the
attack suite and the forthcoming **WiFi Defender** app consume without
duplication.

The important truth this spec starts from: **`wlan_app` already implements most
of the requested attacks.** It has working deauth/disassoc (targeted, unicast,
broadcast, channel-mode), a full Evil Portal (SoftAP + DNS catch-all +
`esp_http_server` + SD templates + cred capture + Karma + NAT bridge), 4-way
handshake + PMKID capture streamed to `.pcap`/hashcat-22000, beacon flood / SSID
spam (funny / rickroll / random / custom), and probe/station recon via the
client scanner. So this is **not greenfield.** The suite is a *consolidation +
gap-fill*, and `wifi_raw` is a *refactor-in-place* of `applications/main/wlan_app/wlan_hal.c`.

Success criteria:

1. `lib/wifi_raw/` exists as a self-contained module (promiscuous RX ring, raw
   802.11 TX, channel hop/lock, mode switch/restore) with **no dependency on any
   app struct**; `wlan_app` is refactored to call it and still builds + behaves
   identically (regression bar: existing deauth / handshake / evil portal / spam
   scenes unchanged in behavior).
2. A new `wifi_attack` app (menu-driven, appid `wifi_attack`) exposes the attack
   set through one coherent UI, gated behind an explicit authorization unlock.
3. Every capture path **streams per-packet to SD** — no full-frame RAM buffering
   anywhere (no-PSRAM discipline, section 8).
4. Attacks that key on injected management frames (deauth, clientless assoc,
   beacon flood) are validated on-hardware against the ESP-IDF mgmt-TX filtering
   risk (section 6), with the documented `esp_wifi_80211_tx` workaround.

---

## 2. Context — what already exists (reuse, do not reinvent)

### 2.1 `applications/main/wlan_app/wlan_hal.{c,h}` — the de-facto raw HAL today
Already provides, on a dedicated pinned worker task (`WlanWorker`, core 0) that
serializes all `esp_wifi_*` calls through a command queue:

- `wlan_hal_start()` / `wlan_hal_stop()` / `wlan_hal_is_started()` — STA-mode
  bring-up. Crucially calls `wlan_hal_release_bt()` first: on this no-PSRAM board
  the BLE controller + Bluedroid hold ~64 KB (with a ~13 KB deinit residual that
  starves `esp_wifi_init`), so BT is fully released and latched off until reboot
  (radios are mutually exclusive here).
- `wlan_hal_set_channel(ch)` (1..14), `wlan_hal_set_promiscuous(enable, cb)` —
  the cb is always re-set (even to NULL) so a stale RX callback from another
  subsystem never leaks across scenes.
- `wlan_hal_send_raw(data, len)` — raw 802.11 TX via `esp_wifi_80211_tx(..., en_sys_seq=true)`,
  **currently capped at 128 bytes** (was 64, grown for the assoc-req RSN IE).
- `wlan_hal_scan()`, `wlan_hal_connect()`, `wlan_hal_get_own_mac()`,
  `wlan_hal_random_mac()` (locally-administered unicast, so the factory MAC never
  goes on air), `wlan_hal_run_in_worker(fn, arg)` generic hook.
- Beacon spam task (`wlan_hal_beacon_spam_*`) with Funny / Rickroll / Random /
  Custom modes, per-5-frame channel rotation.
- Evil Portal (`wlan_hal_evil_portal_*`): SoftAP + DNS hijack task + captive
  `esp_http_server`, cred/valid/busy callbacks, Karma (probe-sniff → spoof
  most-requested SSID), and a NAPT bridge (`set_dns_upstream`) for real internet.

### 2.2 Attack scenes already wired in `wlan_app`
`scene_config.h` already registers: `network_deauth`, `client_picker`,
`handshake` (+ settings + save_path), `pmkid`, `package_sniffer`, `ssid_spam`
(+ run + custom), `evil_portal_menu`/`evil_portal`/`evil_portal_captured`,
`mitm_menu`/`live_creds`. Reference implementations to mine:

- **Deauth** — `scenes/scene_network_deauth.c`. Note the real, tested details:
  26-byte `deauth_tmpl` (subtype `0xC0`; flip byte 0 to `0xA0` for disassoc),
  reason-code rotation `{1,4,6,7,8}`, and **both address directions**
  (AP→STA with RA=STA/TA=BSSID and STA→AP with RA=BSSID/TA=STA) because some APs
  only honor one. Worker is a plain `xTaskCreate` (not FuriThread) so it may call
  `esp_wifi_*` directly; GUI polls status via `volatile` flags in the scene Tick.
- **Handshake/PCAP** — `scenes/scene_handshake.c` + `wlan_pcap.{c,h}`. Per-target
  `File* pcap_file`, `wlan_pcap_open` writes the global header
  (`network = 105` = `LINKTYPE_IEEE802_11`), `wlan_pcap_write_packet` appends one
  record at a time. This is the streaming-to-SD template to copy verbatim.
- **PMKID frame builders** — `pmkid_frames.{c,h}`: `pmkid_build_auth_open()`,
  `pmkid_build_assoc_req()` (RSN IE: CCMP group+pairwise, PSK AKM) — **pure
  functions, host-testable, no esp/furi deps.** The model for all new frame
  builders.
- **Recon** — `wlan_client_scanner.{c,h}`: promiscuous RX → lock-free ring →
  `wlan_client_scanner_drain(app)` merges station MACs + smoothed RSSI into a
  bounded table. `wlan_oui.{c,h}` resolves vendor.
- **GPS wardriver** — external `wardriver` FAP (Cap LoRa-1262). The recon scene
  complements it; it does not replace it.

### 2.3 The authorization-gate precedent
`applications/main/gpio` uses an `app->show_advanced` flag (toggled by
`GpioManualEventToggleAdvanced` in `gpio_scene_manual.c`) to reveal dangerous
pins behind a deliberate action. The attack suite reuses this exact *pattern*
(a boolean unlock the user must deliberately flip), hardened per section 9.

> There is **no** `reverse_shell` under `wlan_app`; it is a separate top-level FAP
> (`applications_user/reverse_shell`, listed in `fam_config.py`). It shares the
> radio via the same STA bring-up but is out of scope here.

---

## 3. Design — attacks (mechanism, UI flow, SD artifacts)

The new app is a thin **orchestration + UI layer** over `wifi_raw` (section 4).
Where `wlan_app` already has a working scene, the attack suite's first
implementation step is to **re-point that scene at `wifi_raw`** and, where it
makes sense, host the consolidated UI in `wifi_attack` while leaving `wlan_app`'s
own menu entries intact (both apps link the shared HAL). Each attack below notes
whether it is *reuse* (exists, re-point) or *new*.

Shared SD root: **`/ext/wifi_attack/`** (portal templates continue to also read
the existing `/ext/wifi/` paths for back-compat). Layout:

```
/ext/wifi_attack/
  captures/   handshake_<ssid>_<ts>.pcap, pmkid.22000, probes_<ts>.csv, recon_<ts>.csv
  portals/    <template>.html  (also reads legacy /ext/apps_data/evil_portal/… )
  creds/      creds_<portal>_<ts>.csv
  wordlists/  ssids.txt
  AUTHORIZED  (optional token file, section 9)
```

### 3.1 Deauth / disassoc  *(reuse — re-point `scene_network_deauth.c`)*
- **Mechanism:** raw `esp_wifi_80211_tx` of 26-byte deauth (`0xC0`) + disassoc
  (`0xA0`) frames via `wifi_raw_tx()`. Three modes, all already proven:
  *targeted* (pick station from client-picker → unicast both directions),
  *broadcast* (RA=`ff:ff:ff:ff:ff:ff`, TA=BSSID), *channel* (scan channel, flood
  every AP found). Reason-code rotation `{1,4,6,7,8}`; TX period ~5 ms.
- **UI flow:** Scan → pick AP (submenu) → optional client-picker for targeted →
  status view (frames sent, target SSID/BSSID, channel; Up/Down = channel ± in
  channel-mode; OK = start/stop; a persistent Auto toggle).
- **SD artifacts:** none (transient attack). Frame counter only.
- **Gate:** requires authorization unlock (section 9) — this is the primary
  jamming-class action.

### 3.2 Evil Portal / captive portal  *(reuse — `wlan_hal_evil_portal_*`)*
- **Mechanism:** rogue SoftAP on chosen SSID/channel → DNS catch-all task
  answers every query with the AP IP → `esp_http_server` serves an HTML clone
  from SD (`%SSID_OPTIONS%` template substitution supported) → submitted
  credentials arrive via `WlanHalEvilPortalCredCb` and are appended to SD.
  Karma mode sniffs probe requests and re-points the SoftAP SSID at the
  most-requested network. Optional NAT bridge (`set_dns_upstream`) gives the
  victim real internet after capture to reduce suspicion.
- **UI flow:** pick template (builtin Google / builtin Router / SD template list,
  rescanned on menu enter) → set SSID (or enable Karma) → optional bridge
  SSID/pass → run view (client count, cred count, last user) → captured view
  (scrollable creds).
- **SD artifacts:** portal HTML read from `/ext/wifi_attack/portals/*.html`;
  captured creds appended to `/ext/wifi_attack/creds/creds_<portal>_<ts>.csv`
  (`ts,user,pwd,client_mac`). **HTML is streamed from the open SD file into the
  httpd response — never fully resident** (section 8).
- **RAM note:** portal path already calls `wlan_app_portal_views_free()` to
  reclaim ~10–15 KB for SoftAP+httpd+DNS; the attack app keeps this discipline
  via an equivalent view-free hook.

### 3.3 WPA/WPA2 handshake + PMKID capture  *(reuse — `scene_handshake.c` + `pmkid`)*
- **Mechanism:** promiscuous sniff on the target channel via `wifi_raw` ring;
  parse 802.11 → EAPOL (`wlan_handshake_parser`); on M1 extract PMKID
  (`wlan_hs_extract_pmkid`), on M1..M4 write to pcap. **Clientless PMKID**:
  inject `pmkid_build_auth_open` + `pmkid_build_assoc_req` (RSN IE requests PMK
  caching) so the AP emits M1 addressed to our STA MAC — no client needed;
  deauth-assisted passive capture is the automatic fallback (per the approved
  2026-07-08 PMKID spec).
- **UI flow:** scan → pick AP (or channel-mode) → status view (EAPOL count,
  deauth count, PMKID hex on success, save path).
- **SD artifacts:**
  - Per-target `.pcap` (`LINKTYPE_IEEE802_11 = 105`) via `wlan_pcap_*`,
    **one record appended per frame** — the canonical no-PSRAM stream.
  - `pmkid.22000` (hashcat `-m 22000`: `WPA*01*<pmkid>*<ap>*<sta>*<essid_hex>***`)
    appended by `wlan_pmkid_out.*`.
  - **`.hccapx` (new, optional):** add `wifi_raw`-adjacent writer
    `wlan_hccapx.{c,h}` that, once all four M-frames + the beacon/assoc SSID are
    seen for a (AP,STA) pair, emits one 393-byte hccapx record streamed to
    `handshake_<ssid>_<ts>.hccapx`. Assembled from a **bounded per-target struct
    (nonces + MICs + eapol frame, a few hundred bytes)**, not a frame buffer.
    Flagged as a stretch artifact; `.pcap` remains the primary output.

### 3.4 Beacon flood / SSID spam  *(reuse — `wlan_hal_beacon_spam_*`; new: wordlist + clone)*
- **Mechanism:** background task TXes beacon frames with random locally-
  administered MACs; SSID source = Random, builtin lists, Custom counter, or
  **new modes**: *Clone-target* (beacon a scanned AP's SSID list to sow evil-twin
  confusion) and *Wordlist* (stream lines from `/ext/wifi_attack/wordlists/ssids.txt`).
  Channel rotates every 5 frames (1..11).
- **UI flow:** pick mode → (wordlist: pick file; custom: text_input; clone: pick
  from scan) → run view (frame count, current SSID).
- **SD artifacts:** wordlist read-only, **line-streamed** (open file, read next
  SSID on demand; never load the whole list). No output file.

### 3.5 Probe-request sniff + station/SSID recon  *(reuse `client_scanner`; new: probe log)*
- **Mechanism:** promiscuous RX; classify probe-request frames → extract
  requested SSID + source station; classify data/mgmt → station↔BSSID
  association. Bounded in-RAM tables (reuse `WLAN_APP_MAX_DEVICES`/`_DEAUTH_CLIENTS`
  sizing) with **spill-to-SD** when full. Feeds evil-twin target selection
  (a probed SSID with a nearby client is a prime Karma/portal target).
- **UI flow:** channel-lock or hop → live list (station MAC, vendor via
  `wlan_oui`, RSSI, probed-SSID) → select-to-target hand-off into deauth/portal.
- **SD artifacts:**
  - `probes_<ts>.csv` — `ts,station_mac,vendor,rssi,channel,requested_ssid`,
    appended per probe (dedup within a small ring, not a full history in RAM).
  - `recon_<ts>.csv` — station↔AP associations, appended as discovered.

### 3.6 Stretch (flag feasibility, do not commit)
- **Karma / rogue-AP auto-respond** — partially exists (portal Karma). Full
  auto-respond (answer *any* probe with a matching assoc) needs a SoftAP that
  changes SSID per probe within DTIM timing; feasible but timing-sensitive.
- **WPA3 SAE-flood** — TX crafted SAE commit frames to exhaust the AP's SAE
  state. **High feasibility risk** (section 6): SAE commit is >128 B (exceeds the
  raw-TX cap), needs a scalar+element computed per frame, and IDF may drop it.
- **DIAL / Chromecast cast hijack** — needs STA association to the victim LAN +
  SSDP/DIAL HTTP; orthogonal to the raw radio, lower priority. Would live as its
  own scene using `wlan_hal_connect` + `esp_http_client`.

---

## 4. The shared `wifi_raw` HAL — interfaces

**Location:** `lib/wifi_raw/wifi_raw.{c,h}`, included as `#include <wifi_raw/wifi_raw.h>`.

**Why here (not `applications/main/lib/`):** the repo already keeps cross-app
shared firmware modules under the root `lib/` (e.g. `lib/subghz`, `lib/drivers`,
`lib/bit_lib`, `lib/toolbox`), and that directory is already on the global
include path (apps do `#include <bit_lib/bit_lib.h>`). A new `lib/wifi_raw/`
matches that established convention, is reachable by **both** `wlan_app` and
`wifi_attack` (and the future Defender) with zero extra include wiring, and keeps
the HAL out of any single app's namespace. Apps opt in via `fap_libs=["wifi_raw", ...]`
in their `application.fam`; the module is added to the `lib` build in the same
way `lib/subghz` is. (Alternative considered: leave it in `wlan_app` and have
`wifi_attack` reach across — rejected: creates an app→app dependency and blocks
the Defender from using it standalone.)

`wifi_raw` is a **behavior-preserving extraction** of the radio-facing half of
`wlan_hal.c` (the worker task, command queue, promiscuous/TX/channel/mode paths,
BT-release). The STA/DHCP/scan/connect/evil-portal/beacon halves stay in
`wlan_app` and become *callers* of `wifi_raw` (or keep private helpers). No new
threading model — the existing single serializing worker is the model.

### 4.1 Lifecycle & mode
```c
// Bring the radio up in a given base mode, releasing BT first (idempotent).
// Replaces wlan_hal_start(); STA is the default.
typedef enum { WifiRawModeSta, WifiRawModeApSta, WifiRawModeMonitor } WifiRawMode;
bool  wifi_raw_start(WifiRawMode mode);
void  wifi_raw_stop(void);              // tears down, leaves BT released
bool  wifi_raw_is_started(void);
void  wifi_raw_release_bt(void);        // == wlan_hal_release_bt (moved here)

// Save/restore the pre-attack radio configuration so a scene can enter monitor
// mode and return the STA link to how it was. Snapshot is opaque + caller-owned.
typedef struct { WifiRawMode mode; uint8_t channel; bool promisc; bool connected; } WifiRawSnapshot;
void  wifi_raw_snapshot(WifiRawSnapshot* out);
void  wifi_raw_restore(const WifiRawSnapshot* snap);
```

### 4.2 Channel control
```c
void    wifi_raw_set_channel(uint8_t ch);          // 1..14, lock
uint8_t wifi_raw_get_channel(void);
// Optional built-in hopper (background), so every scene need not re-implement it.
void    wifi_raw_hop_start(const uint8_t* channels, uint8_t n, uint16_t dwell_ms);
void    wifi_raw_hop_stop(void);
```

### 4.3 Promiscuous RX — header-only, bounded ring callback
The key no-PSRAM primitive. RX callback copies **only up to a bounded snap
length** (default 256 B: enough for full mgmt/EAPOL headers + PMKID KDE, far less
than a 2346 B max frame) into a fixed, preallocated ring. **No allocation in the
callback**; consumers drain from a task context.
```c
typedef struct {
    uint32_t timestamp_us;
    uint16_t orig_len;      // real frame length on air
    uint16_t len;           // bytes actually captured (<= WIFI_RAW_SNAP_LEN)
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  data[/*WIFI_RAW_SNAP_LEN*/]; // flexible; ring stores fixed slots
} WifiRawPkt;

#define WIFI_RAW_SNAP_LEN   256
#define WIFI_RAW_RING_SLOTS 16   // 16 * (256 + hdr) ≈ 4.3 KB, internal RAM

typedef struct WifiRawRing WifiRawRing;
WifiRawRing* wifi_raw_ring_alloc(void);         // one fixed block, caller owns
void         wifi_raw_ring_free(WifiRawRing*);

// Register/unregister promiscuous; filter selects mgmt/data/ctrl subtypes.
typedef enum { WifiRawFilterMgmt=1, WifiRawFilterData=2, WifiRawFilterCtrl=4, WifiRawFilterAll=7 } WifiRawFilter;
void wifi_raw_promisc_start(WifiRawRing* ring, WifiRawFilter filter);
void wifi_raw_promisc_stop(void);

// Drain: pop next packet into caller buffer; returns false when empty. Call from
// a task/Tick, never from ISR. Copies out of the ring slot.
bool wifi_raw_ring_pop(WifiRawRing* ring, WifiRawPkt* out, size_t out_cap);
```
This subsumes both `wlan_hal_set_promiscuous(cb)` and the ad-hoc per-scene rings
in `scene_handshake.c` / `wlan_client_scanner.c` — those become thin consumers of
one shared, audited ring implementation.

### 4.4 Raw 802.11 TX
```c
#define WIFI_RAW_TX_MAX 256   // grown from 128 to admit SAE/large assoc frames
// System fills sequence number (en_sys_seq=true). Returns false on cap/err.
bool wifi_raw_tx(const uint8_t* frame, uint16_t len);
// Convenience wrappers built on wifi_raw_tx + the existing frame builders:
bool wifi_raw_tx_deauth(const uint8_t ra[6], const uint8_t bssid[6], uint8_t reason, bool disassoc);
```
Frame *builders* (deauth, beacon, auth, assoc-req, and future SAE) stay as
**pure, host-testable functions** in `wifi_raw_frames.{c,h}` (folding in the
existing `pmkid_frames.*` and the `deauth_tmpl` logic). `wifi_raw` links them;
`wlan_app` keeps compiling against the same symbols.

### 4.5 Misc moved-in helpers
```c
void wifi_raw_random_mac(uint8_t out[6]);        // == wlan_hal_random_mac
bool wifi_raw_get_own_mac(uint8_t out[6]);
bool wifi_raw_run_in_worker(void (*fn)(void*), void* arg); // generic worker hook
```

**Coexistence contract:** `wifi_raw` owns the single `esp_wifi_*` worker. STA
connect / SoftAP / scan / evil-portal helpers in `wlan_app` must route through
`wifi_raw_run_in_worker()` (as they already do via `wlan_hal_run_in_worker`), so
promiscuous attacks and station mode never touch the driver from two tasks. A
scene that needs monitor mode calls `wifi_raw_snapshot()` on enter and
`wifi_raw_restore()` on exit — clean mode switch + restore.

---

## 5. Components

### 5.1 New shared module — `lib/wifi_raw/`
| File | Purpose |
|---|---|
| `wifi_raw.h` | Public HAL interface (section 4). |
| `wifi_raw.c` | Worker task + cmd queue + promiscuous/TX/channel/mode/BT-release, extracted from `wlan_hal.c`. |
| `wifi_raw_ring.c` | Fixed-slot lock-free RX ring (bounded, no alloc in cb). |
| `wifi_raw_frames.{c,h}` | Pure frame builders: deauth/disassoc, beacon, auth-open, assoc-req(RSN), (stretch) SAE commit. Absorbs `pmkid_frames.*`. |
| `wifi_raw_hop.c` | Optional background channel hopper. |
| `SConscript`/CMake glue | Register as a `lib` module like `lib/subghz` (built into the image; referenced via `fap_libs`). |

### 5.2 New app — `applications/main/wifi_attack/`
| File | Purpose |
|---|---|
| `application.fam` | `App(appid="wifi_attack", name="WiFi Attack", apptype=MENUEXTERNAL, entry_point="wifi_attack_app", stack_size=8*1024, fap_libs=["assets","wifi_raw"], order=68)`. |
| `wifi_attack_app.{c,h}` | App struct, view_dispatcher/scene_manager setup, shared `WifiRawRing*`, authorization flag, SD path helpers. |
| `wifi_attack_icons` / checked-in C icons | **No Pillow in this build** → icons are hand-authored `.c` byte arrays (per the fzcard build memory), not PNGs. |
| `scenes/scene_config.h` | `ADD_SCENE` list (below). |
| `scenes/scene_main.c` | Root menu: Recon / Deauth / Handshake+PMKID / Evil Portal / Beacon Flood / (Advanced…) / Authorize. |
| `scenes/scene_authorize.c` | Authorization unlock gate (section 9). |
| `scenes/scene_scan.c` + `scene_target_pick.c` | Shared AP/station selection (mirrors `wlan_app` connect/client-picker). |
| `scenes/scene_deauth.c` | Re-point of `scene_network_deauth.c` onto `wifi_raw`. |
| `scenes/scene_handshake.c` + `scene_pmkid.c` | Capture scenes; consume `wifi_raw` ring + `wlan_handshake_parser` + pcap/22000/hccapx writers. |
| `scenes/scene_evil_portal*.c` | Portal menu/run/captured (reuse `wlan_hal_evil_portal_*` API, which itself moves to call `wifi_raw`). |
| `scenes/scene_beacon.c` | Beacon flood incl. new wordlist + clone modes. |
| `scenes/scene_recon.c` | Probe/station recon + CSV logging + target hand-off. |
| `views/…` | Status/list canvas views per scene (submenu/widget/variable_item_list/text_input reused from framework; custom canvas views for live counters mirroring `wlan_deauther_view` etc.). |
| `wifi_attack_pcap.*` / `wifi_attack_hccapx.*` / `wifi_attack_csv.*` | SD stream writers (pcap reuses `wlan_pcap.*`; hccapx + csv are new, both append-per-record). |

`scene_config.h` (initial): `main, authorize, scan, target_pick, deauth,
handshake, pmkid, beacon, recon, evil_portal_menu, evil_portal, evil_portal_captured`.

### 5.3 Modified existing files
| File | Change |
|---|---|
| `applications/main/wlan_app/wlan_hal.{c,h}` | Gut the radio primitives; re-implement `wlan_hal_*` as thin inline forwarders to `wifi_raw_*` (or delete callers and switch scenes directly). **Behavior-preserving** — regression-gated. |
| `applications/main/wlan_app/scene_handshake.c`, `scene_network_deauth.c`, `wlan_client_scanner.c`, `pmkid_frames.*` | Replace private rings / `deauth_tmpl` / builders with the shared `wifi_raw` equivalents. |
| `applications/main/wlan_app/application.fam` | Add `wifi_raw` to `fap_libs`. |
| `fam_config.py` | Add `"wifi_attack"` to `APPS`; add `"wifi_raw"` to the lib build list if libs are enumerated there. **APPS is not auto-globbed — this registration is mandatory or the app won't build in.** |

---

## 6. Feasibility risks + mitigation

1. **ESP-IDF management-frame TX filtering (highest).** `esp_wifi_80211_tx` on
   recent IDF can silently drop crafted mgmt frames (deauth/disassoc/auth/assoc)
   because the driver validates them against the current mode/association state.
   **Evidence it works here:** `wlan_app`'s deauth scene and beacon spam already
   TX mgmt frames successfully with `en_sys_seq=true` while promiscuous is
   enabled — that is the ESP32Marauder / Ghost ESP workaround (promiscuous ON +
   `esp_wifi_80211_tx` from the STA interface). **Mitigation:** keep exactly that
   recipe in `wifi_raw_tx` (promiscuous must be active for injection; document it
   as a precondition). *Validate on first hardware bring-up that deauth actually
   disconnects a lab client* — if a future IDF hardens the filter, fall back to
   the community `esp_wifi_internal_tx`/patched-netstack path and flag for review.
   This is the #1 thing to prove before building the rest.
2. **WPA3 SAE-flood (high — recommend deferring).** SAE commit frames exceed the
   old 128 B cap (mitigated by `WIFI_RAW_TX_MAX=256`), but each needs a valid
   scalar+element (finite-field / EC math) recomputed per frame, and IDF is more
   likely to filter these than plain deauth. **Mitigation:** ship it as a clearly
   labeled experimental scene *after* the core suite; host-test the frame builder;
   accept that some APs/IDF combos will drop it. Do not gate suite delivery on it.
3. **Raw-TX cap growth (low).** `WIFI_RAW_TX_MAX` 128→256 enlarges each queued
   command struct. **Mitigation:** the TX command already copies into a fixed
   buffer; confirm the worker cmd-queue depth (4) × item size still fits the
   16 KB worker stack budget on build (it does at 128; re-check at 256).
4. **Refactor regression (medium).** Extracting `wlan_hal` risks breaking the
   four working attack scenes. **Mitigation:** the extraction is behavior-
   preserving and staged — move code, keep `wlan_hal_*` shims, build + smoke-test
   each `wlan_app` scene *before* deleting shims. Host tests on the pure builders
   pin frame layouts across the move.
5. **No-PSRAM heap for SoftAP + httpd + monitor (medium).** Running promiscuous
   capture and SoftAP concurrently is memory-tight. **Mitigation:** keep the
   existing `portal_views_free` reclaim; the suite never runs monitor-capture and
   evil-portal simultaneously (mutually-exclusive scenes); ring is a single fixed
   4.3 KB block.
6. **Concurrent radio ownership (medium).** Two tasks hitting `esp_wifi_*` = crash
   (the port's `BUGFIXES.md` documents promiscuous/task races). **Mitigation:** all
   driver access funnels through the single `wifi_raw` worker; scenes join their
   worker task in `on_exit` before freeing the ring (copy the deauth scene's
   `while(s_task) vTaskDelay` join + guarded-NULL-ring idiom).

---

## 7. Non-Goals

- **Not** rewriting the working `wlan_app` attacks from scratch — this is
  extraction + consolidation, regression-gated.
- **Not** a second radio driver / no companion ESP — native S3 only.
- **Not** cracking (no on-device hashcat/PMK brute force) — capture only; cracking
  is offline.
- **Not** 5 GHz (S3 radio is 2.4 GHz only).
- **Not** replacing the GPS `wardriver` FAP — recon complements it.
- **No** simultaneous BT + WiFi (radios mutually exclusive; BT is released on
  entry and needs a reboot to return).
- SAE-flood, full Karma auto-respond, and DIAL hijack are **stretch**, not v1
  commitments.

---

## 8. No-PSRAM discipline (explicit)

- **Never buffer a full frame in RAM.** The promiscuous RX callback copies at most
  `WIFI_RAW_SNAP_LEN` (256 B) into a fixed-slot ring — enough for mgmt/EAPOL
  headers + PMKID KDE, never the 2346 B max frame.
- **PCAP/hccapx/CSV stream per-packet to SD.** Reuse `wlan_pcap_write_packet`'s
  append-one-record model; hccapx assembles from a *bounded per-target struct*
  (nonces/MIC/eapol, a few hundred bytes), not a frame log. Only the **current
  channel's** live packet is in flight at a time.
- **Portal HTML streams from the open SD file** into the httpd response; templates
  are never fully resident. Multiple templates = multiple files, opened on demand.
- **Recon tables are bounded** (`WLAN_APP_MAX_DEVICES`-class caps) and **spill to
  SD** (CSV append) when full; RAM holds a small working set + dedup ring only.
- **Beacon wordlist is line-streamed** (read next SSID on demand), never loaded
  whole.
- **One fixed ring**, allocated once per attack scene (`wifi_raw_ring_alloc`) and
  freed on exit — no per-packet `malloc`, no growth.
- **Reclaim feature-view RAM** before SoftAP/httpd (keep `portal_views_free`).

---

## 9. Safety / authorization gating

This is lab / authorized-engagement tooling. Deauth, disassoc, beacon flood, and
SAE-flood are jamming-class and may be restricted by law in many jurisdictions.

- **Explicit unlock, reusing the GPIO `show_advanced` pattern.** The app boots
  with `app->authorized = false`. Passive features (scan, recon, handshake/PMKID
  *sniff*, portal *template preview*) are available immediately; **any frame-
  injecting action** (deauth, disassoc, beacon flood, clientless assoc, SAE,
  live evil-portal SoftAP) is hidden/greyed until the user visits **Authorize**
  and performs a deliberate confirm — a two-step "I am authorized to test this
  network" acknowledgement (mirrors `GpioManualEventToggleAdvanced` flip, but
  requires an explicit OK on a warning widget, not a silent toggle).
- **Optional token file.** If `/ext/wifi_attack/AUTHORIZED` is absent, the unlock
  screen additionally warns and requires a longer-press confirm; presence of the
  file (an engagement marker the operator drops) softens to a single confirm.
  (Advisory, not DRM — it is a speed-bump + audit breadcrumb.)
- **Session-scoped.** Authorization resets to `false` on app exit; never
  persisted, so re-entry always re-prompts.
- **On-air identity.** All injected frames use `wifi_raw_random_mac()`
  (locally-administered unicast) — the device's factory MAC never transmits, and
  the SoftAP identity is spoofed, matching existing `wlan_hal` behavior.
- **Clear labeling.** The attack menu and each run view carry a persistent
  "Authorized testing only" footer; the root menu title distinguishes passive
  vs. active sections.

---

## 10. Testing

### 10.1 Compile gate (host + build)
- **Host unit tests (pure logic, no hardware)** — matches the existing host-test
  harness (`main()`-guarded):
  - `wifi_raw_frames`: deauth/disassoc byte layout (subtype `0xC0`/`0xA0`, RA/TA/
    BSSID offsets, reason byte), beacon SSID/rates/channel tags, `auth_open` (30 B),
    `assoc_req` RSN IE (CCMP+PSK, ≤ cap), overflow returns 0.
  - Ring: push N > slots, assert oldest dropped, `pop` FIFO order, snap-len
    truncation preserves headers, no alloc in the cb path (compile-time: cb has
    no malloc symbol).
  - pcap/hccapx/csv writers fed canned frames → byte-exact file output; `.pcap`
    `network==105`; `.22000` line cracks with `hashcat -m 22000` against a known
    PSK (offline, from a canned M1).
- **Build:** `MSYS_NO_PATHCONV=1 cmd.exe /c "…\build_ci.bat"` with
  `IDF_TOOLS_PATH=~/.espressif` (per fzcard build memory — the stock bat path is
  wrong). Green build of `wlan_app` (post-refactor) **and** `wifi_attack`; confirm
  worker cmd-queue/stack budget unchanged at `WIFI_RAW_TX_MAX=256`.

### 10.2 On-hardware checklist (Cardputer ADV)
1. **BT→WiFi handoff:** launch after using BLE; confirm `release_bt` frees enough
   heap for `esp_wifi_init` (watch the `before wifi_init: free_internal/largest`
   log).
2. **Regression:** existing `wlan_app` deauth / handshake / evil-portal / spam
   scenes behave identically post-extraction.
3. **Deauth (risk #1):** targeted + broadcast + channel modes actually disconnect
   a lab client; frame counter climbs; both address directions honored.
4. **Handshake/PMKID:** capture a real 4-way to `.pcap` (opens in Wireshark as
   802.11); clientless PMKID lands `pmkid.22000` and cracks offline; deauth
   fallback engages on timeout.
5. **Evil Portal:** SoftAP joinable, DNS catch-all redirects, SD template renders,
   creds land in `creds_*.csv`, client/cred counters update; Karma re-points SSID;
   bridge gives real internet.
6. **Beacon flood:** SSIDs appear on a scanner phone; wordlist streams; clone mode
   mirrors a scanned AP.
7. **Recon:** probe requests + stations enumerated with vendor/RSSI; CSVs append;
   spill-to-SD when the table fills; target hand-off into deauth/portal works.
8. **Lifecycle:** enter/exit every attack scene 10× rapidly — no crash, no stuck
   promiscuous, no leaked worker task, heap returns to baseline (the port's known
   failure mode).
9. **Authorization gate:** injecting actions hidden until Authorize; reset on exit.
10. **No-PSRAM stress:** run capture then portal back-to-back; confirm no OOM.

---

## 11. Open items to confirm during implementation

- **Lib build wiring:** confirm exactly how a new `lib/wifi_raw` is registered in
  this ESP-IDF/CMake port (does `fam_config.py`/CMake enumerate `lib/` modules, or
  does `fap_libs` suffice?). Mirror `lib/subghz`.
- **Extraction seam:** whether to keep `wlan_hal_*` shims permanently (smaller
  diff, two names for one thing) or fully migrate `wlan_app` scenes to `wifi_raw_*`
  and delete `wlan_hal` (cleaner, larger diff). Pick per regression risk.
- **App split vs. absorb:** decide whether `wifi_attack` *hosts* consolidated UI
  while `wlan_app` keeps its own attack menu entries (both link `wifi_raw`), or
  whether attack scenes move out of `wlan_app` entirely. Default: **coexist** —
  lowest risk, both consume the shared HAL.
- **hccapx scope:** ship in v1 or defer (‎.pcap + .22000 already cover hashcat).
- **Icons:** author the `wifi_attack` menu icons as checked-in `.c` byte arrays
  (no Pillow in this build).
- **`WIFI_RAW_SNAP_LEN`/`RING_SLOTS` tuning:** validate 256×16 headroom against
  the tightest concurrent scene (recon + CSV) on real heap.
- **Menu order / appid collisions:** confirm `order=68` and `appid="wifi_attack"`
  don't collide with existing entries in `fam_config.py`.

---

## Note on version control

This project directory is **not** a git repository (downloaded release
`Flipper-Zero-meets-M5Stack-Cardputer-1.15`), so the "commit the design doc" step
does not apply. The spec lives at
`docs/superpowers/specs/2026-07-11-wifi-attack-suite-design.md`.
