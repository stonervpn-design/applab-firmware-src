# Fox-Hunt Target Picker — Design Spec

**Date:** 2026-07-21
**Component:** `applications/main/ble_spam` (the Bluetooth app's Tracker feature)
**Target hardware:** M5Stack Cardputer ADV (ESP32-S3, has microSD) + StickS3 sibling (SD-less)
**Status:** Approved design — ready for implementation plan

---

## 1. Goal

Turn the BLE tracker into a **target-picker fox hunt**: when you open the Tracker,
first pick *what kind* of thing to hunt; the scan list then shows only that target
type (across all its variants); select a device and geiger-hunt it. Alerts are
logged to the SD card.

Success criteria:

1. `Bluetooth → Tracker` opens a **target picker** menu (not the scan list directly).
2. Categories: **Everything, Apple, Samsung, Tile, Meta, Drone, Bug Sweep**. Each
   catches *all variants* of that type (e.g. Apple = AirTag + AirPods + iPhone/Watch;
   Meta = any mfg-0x058E device regardless of MAC/UUID randomization).
3. Selecting a category opens the existing scan list **filtered to only that
   category**; `Back` returns to the picker to re-choose.
4. The existing FOLLOW!-escalation alert (LED blink + beep) and FOLLOW-first sort
   still work, now over the filtered set.
5. Selecting a device opens the existing geiger locator (unchanged).
6. **Alerts-only SD logging:** each *new* FOLLOW! alert appends one CSV row
   (`/ext/ble/tracker_hunt.csv`). No SD (StickS3) → logging silently disabled, no
   error, feature otherwise identical.
7. Both build targets (Cardputer ADV + StickS3) build clean.

Non-goals (v1): no per-sighting logging (alerts only), no logging toggle (always-on
when SD present), no changes to the geiger scene, no new alert types, no settings UI
(category is the picker; thresholds stay compile-time).

---

## 2. Background — what already exists

`applications/main/ble_spam/`:

- **`ble_tracker_hal.c/.h`** — passive NimBLE scan; `classify_advert()` →
  `TrackerKind` { Unknown, AirTag, SmartTag, Tile, ApplePP, AppleNearby,
  MetaGlasses, DroneRID }; per-device `rssi/best_rssi/first_seen_ms/last_seen_ms`,
  deduped by MAC (`TRACKER_MAX_DEVICES`). `ble_tracker_hal_set_sweep(bool)` toggles
  capture of Unknown devices (Bug Sweep). `ble_tracker_kind_label()`.
- **`tracker_follow.c/.h`** — pure `tracker_follow_state()` NEW→PRESENT→SUSPECT→
  FOLLOWING escalation.
- **`scenes/scene_tracker_scan.c`** — the scan list. Tick pulls
  `ble_tracker_hal_get_devices()`, sorts FOLLOW!-first then RSSI, re-anchors the
  selection by MAC, and fires a **debounced FOLLOW! alert** (`fire_alert()`:
  `furi_hal_light_blink_start(LightRed,…)` + `furi_hal_speaker` 880 Hz beep,
  countdown-stopped). Up/Down select; OK → geiger; **Left/Right toggles
  Trackers↔Bug-Sweep** (`s_sweep_mode`). Alerted-MAC set with prune-on-stale so a
  returning device can re-alert.
- **`views/tracker_list_view.c/.h`** — `TrackerListModel { devices[], count,
  selected, window_offset, now_ms, sweep_mode }`; draws kind/name, follow badge,
  RSSI.
- **`scenes/scene_tracker_geiger.c`** — RSSI proximity locator on
  `app->tracker_target`. Unchanged.
- **`ble_auto_walk_log.c/.h`** — the SD-CSV logging pattern to mirror:
  `furi_record_open(RECORD_STORAGE)` → `storage_simply_mkdir("/ext/ble")` →
  `storage_file_open(…, FSAM_WRITE, FSOM_OPEN_APPEND)`, writes header if new,
  **returns NULL on failure** so every subsequent write is a guarded no-op. Path
  constants live in the `.h`.
- Reached from `scenes/scene_main.c` → **"Tracker"** menu item →
  `BleSpamSceneTrackerScan`.

---

## 3. Design

### 3.1 Target category — `tracker_category.h` (new, pure)

```c
typedef enum {
    TrackerCatEverything = 0,  // all known kinds
    TrackerCatApple,           // AirTag + ApplePP + AppleNearby
    TrackerCatSamsung,         // SmartTag
    TrackerCatTile,            // Tile
    TrackerCatMeta,            // MetaGlasses
    TrackerCatDrone,           // DroneRID
    TrackerCatBugSweep,        // Unknown (sweep mode)
    TrackerCatCount,
} TrackerCategory;

const char* tracker_category_label(TrackerCategory c);   // "Everything","Apple",...
bool tracker_category_uses_sweep(TrackerCategory c);      // true only for BugSweep
// Does a classified device kind belong in this category's list?
bool tracker_category_matches(TrackerCategory c, TrackerKind kind);
```

`tracker_category_matches`:
- `Everything` → any kind except `Unknown`.
- `Apple` → `AirTag || ApplePP || AppleNearby`.
- `Samsung` → `SmartTag`; `Tile` → `Tile`; `Meta` → `MetaGlasses`; `Drone` → `DroneRID`.
- `BugSweep` → `Unknown`.

Pure (only `<stdbool.h>` + `ble_tracker_hal.h` for the `TrackerKind` enum) → the
matcher + labels are host-testable.

### 3.2 App state — `ble_spam_app.h`

Add one field to `BleSpamApp`:

```c
TrackerCategory tracker_category;   // chosen in the picker, read by the scan scene
```

### 3.3 Picker scene — `scenes/scene_tracker_pick.c` (new)

A `Submenu` (reuse `app->submenu`, like `scene_main`). `on_enter`: add one item per
`TrackerCategory` (`tracker_category_label`), switch to `BleSpamViewSubmenu`.
`on_event`: on selection, set `app->tracker_category = index` and
`scene_manager_next_scene(BleSpamSceneTrackerScan)`. `on_exit`: `submenu_reset`.
Back from here pops to the main menu (normal scene stack).

Menu wiring: `scene_main.c` "Tracker" item now routes to `BleSpamSceneTrackerPick`
instead of `BleSpamSceneTrackerScan`.

### 3.4 Scan scene changes — `scene_tracker_scan.c`

- **Sweep from category:** `on_enter` sets
  `ble_tracker_hal_set_sweep(tracker_category_uses_sweep(app->tracker_category))`
  instead of hardcoded `false`. Drop the static `s_sweep_mode` and the **Left/Right
  toggle branch** (category is chosen in the picker now).
- **Filter in the Tick:** when copying `ble_tracker_hal_get_devices()` into the
  model, include only devices where
  `tracker_category_matches(app->tracker_category, dev.kind)`. (Bug Sweep: the HAL
  already returns only Unknowns in sweep mode, and the matcher passes Unknown.) Sort,
  re-anchor selection, alert, all as today — over the filtered array.
- **Header:** set `model->sweep_mode` as today (view still shows the mode line); the
  filtered count reflects the category. (Optional: pass a category label to the view
  for the header — see §3.6.)
- **Logging:** `on_enter` opens `tracker_log_open()` (stores handle on the app or a
  file-static); `on_exit` closes it. In the FOLLOW!-alert loop, right after
  `alerted_add()` + `fire_alert()` for a *newly* following device, call
  `tracker_log_alert(log, app->tracker_category, dev)`. NULL log → no-op.
- **Back** now pops to the picker (natural), where the user can pick another target.

### 3.5 Logging module — `tracker_log.c/.h` (new)

Mirror `ble_auto_walk_log` closely (same storage lifecycle + NULL-on-no-SD guard).

```c
#define TRACKER_LOG_DIR  "/ext/ble"
#define TRACKER_LOG_PATH "/ext/ble/tracker_hunt.csv"

typedef struct TrackerLog TrackerLog;

TrackerLog* tracker_log_open(void);      // mkdir+append; header if new; NULL on no-SD
void tracker_log_close(TrackerLog* log);
// One row on a new FOLLOW! alert. No-op if log==NULL.
void tracker_log_alert(TrackerLog* log, TrackerCategory cat, const TrackerDevice* dev);
```

CSV header: `timestamp,category,kind,address,name,best_rssi,event\n`. Row per alert:
`furi_get_tick()`, `tracker_category_label(cat)`, `ble_tracker_kind_label(dev->kind)`,
`AA:BB:CC:DD:EE:FF`, quoted name (reuse the auto-walk quoting helper style),
`dev->best_rssi`, `FOLLOW`. Reuses the guarded-write / quoted-field pattern from
`ble_auto_walk_log.c` (name field CSV-escaped, silent on write failure).

### 3.6 List view — `tracker_list_view.c/.h` (minimal)

No structural change required. Optional polish: add a `const char* category_label`
to `TrackerListModel` and draw it in the header instead of the generic
Trackers/Bug-Sweep string, so the header reads e.g. "Meta (3)". If this complicates
the diff, keep the existing header and skip — not a success criterion.

### 3.7 Geiger — direction-finding aid (added per user request)

`scene_tracker_geiger.c` + `tracker_geiger_view.c/.h` gain an RSSI **direction-finding
aid** (the geiger already hunts `app->tracker_target` by RSSI proximity; this makes it
show *which way*). True BLE bearing (AoA/AoD) is impossible on the ESP32-S3 (no antenna
array / CTE), and RSSI is scalar — so this is a **human-in-the-loop** aid using the
body-shield technique (hold device to chest, rotate; body blocks the far side so RSSI
peaks toward the target).

Per scene tick, from the target's live RSSI:
- **Smoothed RSSI** — integer EMA `smooth = (2*smooth + raw)/3` (drives the label, bar,
  and the proximity beep, killing jitter).
- **Peak-hold** — tracks the strongest smoothed RSSI, instant on rise, **~1 dB/s decay**
  toward the current value so a new sweep re-finds the peak. **OK clears it** for a fresh
  sweep (the geiger view forwards a short OK press as a custom event — its context is
  already the `ViewDispatcher`).
- **Trend** — `+1/-1/0` from the smoothed delta (warmer / colder / steady).

View: proximity label + `dBm` value + **▲/▼ trend arrow**, RSSI bar with a **peak-marker
vertical line**, and a hint (`rotate  OK=reset`). New model fields: `rssi_smooth`,
`rssi_peak`, `trend`. No new hardware; a $2 I2C compass could later add a real
rotate-and-plot bearing (out of scope here).

---

## 4. Testing

**Host unit test** (pure, `gcc`/WSL — like `test_tracker_follow.c`):
`tests/test_tracker_category.c` for `tracker_category_matches` / `_uses_sweep`:
- Everything matches every non-Unknown kind, rejects Unknown.
- Apple matches AirTag/ApplePP/AppleNearby, rejects SmartTag/Tile/Meta/Drone.
- Samsung/Tile/Meta/Drone each match only their one kind.
- BugSweep matches only Unknown; `_uses_sweep` true only for BugSweep.
- `tracker_category_label` returns the expected strings for all categories.

**On-hardware (Cardputer, has SD):**
- Bluetooth → Tracker shows the 7-item picker. Pick **Meta** → list shows only Meta
  glasses (put a pair nearby); pick **Apple** → AirTags/AirPods/iPhone appear, others
  gone; **Bug Sweep** → unknown devices. Back returns to the picker.
- Leave a target near the device → escalates to FOLLOW!: LED blinks, one beep, and a
  row appears in `/ext/ble/tracker_hunt.csv` (category+kind+MAC+rssi+FOLLOW).
- Select a device → geiger beeps faster as you approach.

**On-hardware (StickS3, no SD):** identical behavior minus logging — no crash, no
error; `tracker_log_open()` returns NULL and alerts still fire.

**Build gate:** `build_fix.bat` (Cardputer ADV) via **`clean_build_jul12.bat`**-style
set-target if config is stale, and `build_m5stick_s3.bat` — both reach `Project build
complete` / `SENTINEL_OK`. (See the ledger's PSRAM/sdkconfig note: use PowerShell to
invoke the build `.bat`, verify by mtime + SENTINEL markers + `.obj` existence.)

---

## 5. Risks

1. **Empty filtered list looks broken.** Picking a category with nothing nearby shows
   an empty list. Mitigation: the view already draws "Scanning…" on count 0 — fine;
   the picker made the target explicit so an empty list reads as "none nearby."
2. **Log file handle lifetime.** Must open in `on_enter` and close in `on_exit`
   (both the normal Back and the scene-teardown path) to avoid a leaked file handle /
   locked SD. Mirror auto-walk's open/close discipline; close is idempotent-safe.
3. **SD absent / removed mid-session (StickS3 or pulled card).** `tracker_log_open`
   returns NULL → all writes guarded no-ops. A card pulled *after* open: writes fail
   silently (storage_file_write returns short), no crash — acceptable for v1.
4. **Sweep-mode + filter interaction.** Bug Sweep enables HAL sweep (captures
   Unknowns); the other categories run sweep off (Unknowns not captured) and filter
   by kind. `tracker_category_matches(BugSweep, Unknown)=true` and
   `(non-sweep-cat, Unknown)=false` keep these consistent.
5. **Shared `app->submenu`.** The picker reuses the same Submenu widget as
   `scene_main`; `scene_main` already `submenu_reset`s on exit and the picker resets
   on its own exit — no residual items (same pattern the app already relies on).

---

## 6. File structure

- **New:** `applications/main/ble_spam/tracker_category.h` (+ `.c` if the matcher/
  labels are non-trivial) — `TrackerCategory`, `_label`, `_uses_sweep`, `_matches`.
- **New:** `applications/main/ble_spam/tracker_log.c` / `.h` — alerts-only SD CSV.
- **New:** `applications/main/ble_spam/scenes/scene_tracker_pick.c` — the picker.
- **New:** `tests/test_tracker_category.c` — host unit tests.
- **Modify:** `scenes/scene_config.h` — register `tracker_pick` scene.
- **Modify:** `ble_spam_app.h` — `TrackerCategory tracker_category;`.
- **Modify:** `scenes/scene_main.c` — "Tracker" routes to the picker scene.
- **Modify:** `scenes/scene_tracker_scan.c` — sweep-from-category, filter in Tick,
  drop Left/Right toggle + `s_sweep_mode`, open/close + alert-row logging.
- **Modify (optional):** `views/tracker_list_view.c/.h` — category label in header.
- **Modify:** `scenes/scene_tracker_geiger.c` + `views/tracker_geiger_view.c/.h` —
  direction-finding aid (smoothed RSSI + decaying peak-hold + trend + OK-reset; beep
  tracks the smoothed value). See §3.7.

---

## Note on version control

Repo `Flipper-Zero-meets-M5Stack-Cardputer-1.15`, branch `nimble-port`. Index is
clean; unrelated nrf24/subghz/`fam_config.py` changes remain unstaged working-tree
edits — commit only this feature's paths (no `git add -A`). Spec committed alongside
the plan.
