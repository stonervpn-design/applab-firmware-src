# Tracker / Bug Sweep — Design Spec

**Date:** 2026-07-08
**Component:** `applications/main/ble_spam` (the BLE app's "Tracker" feature)
**Target hardware:** M5Stack Cardputer ADV (ESP32-S3, native BLE; WS2812 RGB LED on GPIO21; ES8311 speaker)
**Status:** Approved design — ready for implementation plan

---

## 1. Goal

Turn the existing brand-classifying BLE tracker scanner into a counter-surveillance
tool by adding:

- **(A) "Following" detection** — flag a tracker that persists near the user over
  time (the anti-stalking signal), using a time-based escalation (no GPS on device).
- **(B) Bug Sweep mode** — capture *all* nearby BLE devices (not just known
  tracker brands), sorted by signal, to hunt a hidden beacon/bug.

Both reuse the existing scan HAL and the existing "geiger" RSSI locator.

Success criteria:

1. A tracker (AirTag/SmartTag/Tile) or a phone left near the device escalates
   `NEW → PRESENT → SUSPECT → FOLLOWING` over ~5 minutes, and a device that leaves
   (goes stale) stops escalating.
2. On escalation to FOLLOWING, the app shows a banner, pulses the RGB LED, and
   emits a short beep (debounced once per device).
3. **Bug Sweep** mode lists every nearby BLE device (known + unknown) sorted with
   FOLLOWING/strongest first; unknown devices show a BLE local name or `mfg 0xXXXX`.
4. Selecting any device (either mode) opens the existing geiger locator and homes
   in on it by RSSI.
5. Enter/exit and mode-toggle leave the BLE scan cleanly stopped; no stuck speaker
   or LED.

Non-goals (v1, deferred):

- No SD logging / history persistence, no GPS / location correlation.
- No changes to the geiger scene beyond it continuing to work for unknown devices.
- No per-threshold settings screen (thresholds are compile-time `#define`s).

---

## 2. Background — what already exists

`applications/main/ble_spam/ble_tracker_hal.c/.h`:

- Passive BLE GAP scan; `classify_advert()` matches manufacturer data / service
  UUIDs into `TrackerKind`: AirTag (Apple 0x004C sub 0x12), Apple Nearby (sub
  0x10), Apple Proximity Pairing / AirPods (sub 0x07, with a model-name table),
  Samsung SmartTag (0x0075), Tile (UUID 0xFEED/0xFD84).
- `TrackerDevice { addr, addr_type, kind, name[24], rssi, best_rssi, last_seen_ms }`,
  deduped by MAC, up to `TRACKER_MAX_DEVICES` (24).
- **Non-matching devices are discarded** (`if(!match) break;`).
- `scene_tracker_scan.c` — Tick pulls `ble_tracker_hal_get_devices()` into
  `TrackerListModel { devices[], count, selected, window_offset }`; Up/Down select;
  OK copies the selection to `app->tracker_target` and opens the geiger scene.
- `scene_tracker_geiger.c` — RSSI-proximity locator; already drives the beeper via
  `furi_hal_speaker_acquire/start(pitch,0.5f)/stop/release`. Reused unchanged.
- Reached from `scene_main.c` → single "Tracker" menu item → `BleSpamSceneTrackerScan`.

---

## 3. Design

### 3.1 HAL extension — `ble_tracker_hal.c/.h`

Add one field to `TrackerDevice`:

```c
uint32_t first_seen_ms;   // furi_get_tick() marking the start of the CURRENT
                          // continuous presence (reset when it returns from stale)
```

- On new device: `first_seen_ms = now;`
- On dedup match: if the device had gone **stale** since its last sighting
  (`now - last_seen_ms >= FOLLOW_STALE_MS`), reset `first_seen_ms = now` — the
  device left and came back, so this is a fresh presence and `dwell` must restart.
  Otherwise leave `first_seen_ms` unchanged (continuous presence). Then update
  `last_seen_ms`/`rssi`/`best_rssi` as today. This makes `dwell = last_seen -
  first_seen` a measure of *continuous* presence, which is what "following" means —
  without needing a separate sighting counter.

**Sweep mode:**

```c
void ble_tracker_hal_set_sweep(bool enable);   // default false
```

- When `false` (default): current behavior — only known-brand matches are stored.
- When `true`: devices that fail `classify_advert()` are ALSO stored with
  `kind = TrackerKindUnknown` and a label derived (in priority order) from:
  1. BLE local name — AD type `0x09` (complete) or `0x08` (shortened), copied into
     `name[]` (truncated to 23 chars).
  2. else `"mfg 0xXXXX"` from manufacturer-data company id (AD type `0xFF`).
  3. else leave `name[0]='\0'` (the view falls back to showing the MAC).
- Toggling sweep clears the device table and restarts accumulation (so the list
  reflects the active mode).

Bump `TRACKER_MAX_DEVICES` 24 → 40. Cost: `sizeof(TrackerDevice)` ≈ 46 B × 40 ≈
1.8 KB static — acceptable. When full, keep existing entries (new unknowns are
dropped, same as today's known-tracker overflow).

A new pure helper for the label parse is testable on host:

```c
/* Extract a BLE local name (AD 0x09/0x08) from an advert TLV buffer into out.
 * Returns true if found. Pure — no esp/furi deps. */
bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz);
```

Put this in the new pure module (§3.2) so the HAL and the tests share it.

### 3.2 Following classifier + AD helpers — `tracker_follow.c/.h` (new, pure)

Pure C (`<stdint.h>`, `<stdbool.h>`, `<string.h>` only) → host-testable, like
`pmkid_frames.c`.

```c
typedef enum {
    FollowNew,        // dwell < FOLLOW_PRESENT_MS
    FollowPresent,    // dwell >= FOLLOW_PRESENT_MS
    FollowSuspect,    // dwell >= FOLLOW_SUSPECT_MS
    FollowFollowing,  // dwell >= FOLLOW_FOLLOWING_MS
} FollowState;

/* Escalate only while ACTIVE (seen within FOLLOW_STALE_MS). A stale device
 * returns FollowNew regardless of dwell (it left, so it isn't "following"). */
FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms);

const char* tracker_follow_label(FollowState s);   // "NEW" / "SEEN" / "SUSPECT" / "FOLLOW!"
```

Thresholds (`#define`, tunable):

```
FOLLOW_STALE_MS       30000    // not seen for 30 s → inactive
FOLLOW_PRESENT_MS     60000    // 1 min dwell
FOLLOW_SUSPECT_MS     180000   // 3 min dwell
FOLLOW_FOLLOWING_MS   300000   // 5 min dwell
```

`dwell = last_seen_ms - first_seen_ms` (both `uint32_t`, modular-safe). If
`now_ms - last_seen_ms >= FOLLOW_STALE_MS` → `FollowNew`.

`ble_adv_extract_local_name()` (§3.1) also lives here.

### 3.3 Scan scene extension — `scene_tracker_scan.c`

- **Mode toggle:** Left/Right key flips `app->tracker_sweep_mode` (new bool on the
  app), calls `ble_tracker_hal_set_sweep(mode)`, and resets selection. The list
  header shows `Trackers` or `Bug Sweep`.
- **Sort + follow-state:** in the Tick handler, after copying devices into the
  model, sort them so `FollowFollowing` entries come first, then by `best_rssi`
  descending. Add `uint32_t now_ms` and `bool sweep_mode` to `TrackerListModel` so
  the view can render per-device follow-state (via `tracker_follow_state`) and the
  mode header without recomputing timing.
- **Alert:** track a per-device "already alerted" set (by MAC, small fixed array).
  When a device transitions into `FollowFollowing` for the first time: set a banner
  string, pulse the RGB LED (`furi_hal_light_set(LightRed|LightGreen|LightBlue,…)`
  briefly, or a short blink sequence), and emit a short beep the same way the
  geiger scene does (`furi_hal_speaker_acquire(100)` → `furi_hal_speaker_start(880,
  0.5f)` → after ~150 ms `stop`/`release`). Debounced: alert fires once per device
  until it goes stale and returns.

### 3.4 List view — `tracker_list_view.c/.h`

- Extend `TrackerListModel` with `uint32_t now_ms;` and `bool sweep_mode;`.
- Draw: header shows the mode; each row shows kind/name (or MAC for nameless
  unknowns), a follow-state badge (`NEW/SEEN/SUSPECT/FOLLOW!`), dwell time, and the
  existing RSSI. A FOLLOWING row is visually emphasized (e.g. inverted).

### 3.5 Geiger locator — unchanged

`scene_tracker_geiger.c` already homes in on `app->tracker_target` by RSSI and
works for any `TrackerDevice`, including `TrackerKindUnknown`. No change required.

### 3.6 Menu — unchanged

Single "Tracker" entry stays; Bug Sweep is reached by the in-scene Left/Right
toggle. No `scene_main.c` change.

---

## 4. Testing

**Host unit tests** (pure logic, `gcc`/WSL, no hardware — like `pmkid_frames`):
- `tracker_follow_state`: dwell just below/above each threshold → correct state;
  a stale `last_seen` (now - last_seen ≥ 30 s) → `FollowNew` even at 10-min dwell;
  modular-wrap safety of the dwell subtraction.
- `ble_adv_extract_local_name`: an advert with a `0x09` name tag → extracted and
  truncated to `out_sz`; `0x08` shortened name → extracted; no name tag → false;
  malformed/over-long field length → no over-read, returns false.

**On-hardware:**
- Leave a phone/AirTag near the device: badge escalates NEW→…→FOLLOW! over ~5 min;
  at FOLLOW! the banner shows, LED pulses, one beep sounds; walking the tracker
  away (stale) stops escalation.
- Toggle to Bug Sweep: many BLE devices appear (unknowns labeled by name or
  `mfg 0xXXXX`), sorted strongest-first; toggle back returns to known-only.
- Select any device → geiger beeps faster as you approach it.
- Enter/exit + toggle repeatedly: scan stops cleanly, no stuck speaker/LED.

---

## 5. Risks

1. **Device-table saturation** in busy RF areas (40 cap). Mitigation: keep existing
   entries, surface a "full" hint in the header; strongest float to top via sort.
2. **Speaker contention**: the alert beep and the geiger scene both use
   `furi_hal_speaker`. The alert only runs in the scan scene (geiger is a different
   scene), and it acquires/releases briefly — no overlap. Guard the beep with
   `furi_hal_speaker_acquire(timeout)` and skip if not acquired.
3. **Alert spam**: without per-device debounce the LED/beep would fire every tick.
   The "already-alerted" set (cleared when a device goes stale) prevents this.
4. **GAP callback ownership**: `ble_tracker_hal_start_scan` already takes over the
   GAP callback from `ble_walk_hal`; the sweep toggle must not re-register in a way
   that races the running scan — it only flips a flag + clears the table, it does
   not re-init the stack.

---

## 6. File structure

- **New:** `applications/main/ble_spam/tracker_follow.c` / `.h` — pure follow-state
  classifier + `ble_adv_extract_local_name`.
- **New:** `tests/test_tracker_follow.c` — host unit tests.
- **Modify:** `ble_tracker_hal.c/.h` — `first_seen_ms` field + return-from-stale
  reset, sweep mode + unknown-device capture, `TRACKER_MAX_DEVICES` 24→40.
- **Modify:** `views/tracker_list_view.c/.h` — model `now_ms`/`sweep_mode` fields;
  draw follow-state badge, dwell, mode header, unknown labels.
- **Modify:** `scenes/scene_tracker_scan.c` — mode toggle, sort, follow-state
  computation, FOLLOWING alert (banner + LED + beep, debounced).
- **Modify:** `ble_spam_app.h` — add `bool tracker_sweep_mode;` (and any alert
  bookkeeping) to `BleSpamApp`.

---

## Note on version control

Same repo as the PMKID feature (`Flipper-Zero-meets-M5Stack-Cardputer-1.15`, now a
git repo on `master`). Spec committed alongside the plan.
