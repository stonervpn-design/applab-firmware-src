# Fox-Hunt Target Picker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a target-category picker to the ble_spam BLE tracker: pick what to fox-hunt (Everything/Apple/Samsung/Tile/Meta/Drone/BugSweep), the scan list filters to that type across all variants, alerts log to SD.

**Architecture:** A new picker scene (reusing the app's existing Submenu) routed to before the existing scan scene; the scan scene filters the HAL device list by the chosen category and logs FOLLOW! alerts. A small pure `tracker_category` module (host-tested) does category→kind mapping; a `tracker_log` module (mirror of `ble_auto_walk_log`) does alerts-only SD CSV.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), Flipper scene manager + view dispatcher, `furi` storage (`RECORD_STORAGE`), host unit tests via `gcc` under WSL.

## Global Constraints

- New `.c` files under `applications/main/ble_spam/` auto-compile (build globs the app tree) — no sources list to edit.
- Pure module (`tracker_category.c`) uses only `<stdbool.h>` + `ble_tracker_hal.h` (which is pure: `<stdbool.h>`/`<stdint.h>` only) — no esp/furi — so it host-compiles.
- `tracker_log` must return NULL when the SD/storage open fails (StickS3 is SD-less); every log call is then a guarded no-op. No crash, no error UI.
- **Commit staging:** the index is clean but the working tree has unrelated modified files (nrf24/subghz/fam_config). Commit ONLY this feature's explicit paths via a path-scoped commit (`git commit -m "…" -- <paths>`). NEVER `git add -A`.
- **Builds:** invoke build `.bat` via the **PowerShell tool** (`cmd /c "`"<root>\build_fix.bat`""`), NOT the Bash tool's `cmd.exe /c` (MSYS mangles args → silent no-op). Verify success by `build_fix.log`/`build_m5stick_s3.log` **mtime advancing** + `SENTINEL_OK` marker + the new `.obj` files existing — never a log tail alone. Config is currently correct (esp32s3, SPIRAM=n); a plain `build_fix.bat` (incremental) is fine. Only if a build configures for the wrong chip or SPIRAM turns on, use `clean_build_jul12.bat` (does `set-target esp32s3`).
- Git Bash has no host `gcc` — host tests run via WSL: `wsl.exe -- bash -lc "cd /mnt/c/... && gcc ... && ./out"`.
- **Branch:** `nimble-port`.

---

## File Structure

- **New** `applications/main/ble_spam/tracker_category.h` / `.c` — `TrackerCategory` enum, `_label`, `_uses_sweep`, `_matches`.
- **New** `tests/test_tracker_category.c` — host unit tests.
- **New** `applications/main/ble_spam/tracker_log.h` / `.c` — alerts-only SD CSV (mirror `ble_auto_walk_log`).
- **New** `applications/main/ble_spam/scenes/scene_tracker_pick.c` — the picker scene.
- **Modify** `applications/main/ble_spam/scenes/scene_config.h` — register the picker scene.
- **Modify** `applications/main/ble_spam/ble_spam_app.h` — `#include "tracker_category.h"` + `TrackerCategory tracker_category;` field.
- **Modify** `applications/main/ble_spam/scenes/scene_main.c` — "Tracker" routes to the picker.
- **Modify** `applications/main/ble_spam/scenes/scene_tracker_scan.c` — category-driven sweep, filter in Tick, drop Left/Right toggle + `s_sweep_mode`, open/close + alert logging.

---

## Task 1: Pure target-category module (host-tested)

**Files:**
- Create: `applications/main/ble_spam/tracker_category.h`
- Create: `applications/main/ble_spam/tracker_category.c`
- Test: `tests/test_tracker_category.c`

**Interfaces:**
- Consumes: `TrackerKind` (from `ble_tracker_hal.h`).
- Produces:
  - `typedef enum { TrackerCatEverything=0, TrackerCatApple, TrackerCatSamsung, TrackerCatTile, TrackerCatMeta, TrackerCatDrone, TrackerCatBugSweep, TrackerCatCount } TrackerCategory;`
  - `const char* tracker_category_label(TrackerCategory c);`
  - `bool tracker_category_uses_sweep(TrackerCategory c);`
  - `bool tracker_category_matches(TrackerCategory c, TrackerKind kind);`

- [ ] **Step 1: Write the header**

Create `applications/main/ble_spam/tracker_category.h`:

```c
#pragma once

#include <stdbool.h>
#include "ble_tracker_hal.h"   // TrackerKind

// A fox-hunt target category. Each maps to one or more TrackerKind values so it
// catches all variants of that type.
typedef enum {
    TrackerCatEverything = 0,  // all known kinds (not Unknown)
    TrackerCatApple,           // AirTag + AirPods (ApplePP) + iPhone/Watch (Nearby)
    TrackerCatSamsung,         // SmartTag
    TrackerCatTile,            // Tile
    TrackerCatMeta,            // Meta glasses (mfg 0x058E)
    TrackerCatDrone,           // Drone Remote ID
    TrackerCatBugSweep,        // Unknown devices (HAL sweep mode)
    TrackerCatCount,
} TrackerCategory;

const char* tracker_category_label(TrackerCategory c);
// True only for Bug Sweep (the HAL must capture Unknown devices).
bool tracker_category_uses_sweep(TrackerCategory c);
// Does a classified device kind belong in this category's filtered list?
bool tracker_category_matches(TrackerCategory c, TrackerKind kind);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_tracker_category.c`:

```c
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/tracker_category.h"

static void test_everything(void) {
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindAirTag));
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindMetaGlasses));
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindDroneRID));
    assert(!tracker_category_matches(TrackerCatEverything, TrackerKindUnknown));
    printf("test_everything OK\n");
}
static void test_apple(void) {
    assert(tracker_category_matches(TrackerCatApple, TrackerKindAirTag));
    assert(tracker_category_matches(TrackerCatApple, TrackerKindApplePP));
    assert(tracker_category_matches(TrackerCatApple, TrackerKindAppleNearby));
    assert(!tracker_category_matches(TrackerCatApple, TrackerKindSmartTag));
    assert(!tracker_category_matches(TrackerCatApple, TrackerKindMetaGlasses));
    printf("test_apple OK\n");
}
static void test_singles(void) {
    assert(tracker_category_matches(TrackerCatSamsung, TrackerKindSmartTag));
    assert(!tracker_category_matches(TrackerCatSamsung, TrackerKindTile));
    assert(tracker_category_matches(TrackerCatTile, TrackerKindTile));
    assert(tracker_category_matches(TrackerCatMeta, TrackerKindMetaGlasses));
    assert(!tracker_category_matches(TrackerCatMeta, TrackerKindAppleNearby));
    assert(tracker_category_matches(TrackerCatDrone, TrackerKindDroneRID));
    assert(!tracker_category_matches(TrackerCatDrone, TrackerKindTile));
    printf("test_singles OK\n");
}
static void test_sweep(void) {
    assert(tracker_category_matches(TrackerCatBugSweep, TrackerKindUnknown));
    assert(!tracker_category_matches(TrackerCatBugSweep, TrackerKindAirTag));
    assert(tracker_category_uses_sweep(TrackerCatBugSweep));
    assert(!tracker_category_uses_sweep(TrackerCatEverything));
    assert(!tracker_category_uses_sweep(TrackerCatMeta));
    printf("test_sweep OK\n");
}
static void test_labels(void) {
    assert(strcmp(tracker_category_label(TrackerCatEverything), "Everything") == 0);
    assert(strcmp(tracker_category_label(TrackerCatApple), "Apple") == 0);
    assert(strcmp(tracker_category_label(TrackerCatMeta), "Meta Glasses") == 0);
    assert(strcmp(tracker_category_label(TrackerCatBugSweep), "Bug Sweep") == 0);
    printf("test_labels OK\n");
}
int main(void) {
    test_everything(); test_apple(); test_singles(); test_sweep(); test_labels();
    printf("ALL TRACKER CATEGORY TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/tcat tests/test_tracker_category.c applications/main/ble_spam/tracker_category.c && /tmp/tcat"
```
Expected: FAILS to compile/link — `tracker_category.c` does not exist yet.

- [ ] **Step 4: Write the implementation**

Create `applications/main/ble_spam/tracker_category.c`:

```c
#include "tracker_category.h"

const char* tracker_category_label(TrackerCategory c) {
    switch(c) {
    case TrackerCatEverything: return "Everything";
    case TrackerCatApple:      return "Apple";
    case TrackerCatSamsung:    return "Samsung";
    case TrackerCatTile:       return "Tile";
    case TrackerCatMeta:       return "Meta Glasses";
    case TrackerCatDrone:      return "Drone RID";
    case TrackerCatBugSweep:   return "Bug Sweep";
    default:                   return "?";
    }
}

bool tracker_category_uses_sweep(TrackerCategory c) {
    return c == TrackerCatBugSweep;
}

bool tracker_category_matches(TrackerCategory c, TrackerKind kind) {
    switch(c) {
    case TrackerCatEverything:
        return kind != TrackerKindUnknown;
    case TrackerCatApple:
        return kind == TrackerKindAirTag || kind == TrackerKindApplePP ||
               kind == TrackerKindAppleNearby;
    case TrackerCatSamsung:  return kind == TrackerKindSmartTag;
    case TrackerCatTile:     return kind == TrackerKindTile;
    case TrackerCatMeta:     return kind == TrackerKindMetaGlasses;
    case TrackerCatDrone:    return kind == TrackerKindDroneRID;
    case TrackerCatBugSweep: return kind == TrackerKindUnknown;
    default:                 return false;
    }
}
```

- [ ] **Step 5: Run the test to verify it passes**

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/tcat tests/test_tracker_category.c applications/main/ble_spam/tracker_category.c && /tmp/tcat"
```
Expected: five `... OK` lines then `ALL TRACKER CATEGORY TESTS PASSED`, exit 0, no warnings.

- [ ] **Step 6: Commit**

```
git add applications/main/ble_spam/tracker_category.h applications/main/ble_spam/tracker_category.c tests/test_tracker_category.c
git commit -m "feat(ble_spam): pure tracker target-category matcher + labels (host-tested)"
```

---

## Task 2: Picker scene, SD alert-log, scan filter, wiring (firmware builds green)

**Files:**
- Create: `applications/main/ble_spam/tracker_log.h`
- Create: `applications/main/ble_spam/tracker_log.c`
- Create: `applications/main/ble_spam/scenes/scene_tracker_pick.c`
- Modify: `applications/main/ble_spam/scenes/scene_config.h`
- Modify: `applications/main/ble_spam/ble_spam_app.h`
- Modify: `applications/main/ble_spam/scenes/scene_main.c`
- Modify: `applications/main/ble_spam/scenes/scene_tracker_scan.c`

**Interfaces:**
- Consumes (Task 1): `TrackerCategory`, `tracker_category_label/_uses_sweep/_matches`.
- Consumes (existing): `TrackerDevice`/`TrackerKind`/`ble_tracker_kind_label` (`ble_tracker_hal.h`), `TrackerListModel` (`views/tracker_list_view.h`), `ble_auto_walk_log.c` as the storage pattern reference, `app->submenu`, `BleSpamViewSubmenu`, scene macros.
- Produces: `TrackerLog`/`tracker_log_open/_close/_alert`; scene handlers `ble_spam_scene_tracker_pick_on_enter/_on_event/_on_exit`; app field `tracker_category`; enum `BleSpamSceneTrackerPick`.

- [ ] **Step 1: Write the SD alert-log header**

Create `applications/main/ble_spam/tracker_log.h`:

```c
#pragma once

#include "tracker_category.h"
#include "ble_tracker_hal.h"   // TrackerDevice

#define TRACKER_LOG_DIR  "/ext/ble"
#define TRACKER_LOG_PATH "/ext/ble/tracker_hunt.csv"

typedef struct TrackerLog TrackerLog;

// Opens (creates) the CSV in append mode, writing the header if new. Returns NULL
// if storage is unavailable (e.g. SD-less StickS3) — callers then no-op on log.
TrackerLog* tracker_log_open(void);
void tracker_log_close(TrackerLog* log);

// One CSV row for a new FOLLOW! alert. No-op if log==NULL.
void tracker_log_alert(TrackerLog* log, TrackerCategory cat, const TrackerDevice* dev);
```

- [ ] **Step 2: Write the SD alert-log implementation**

Create `applications/main/ble_spam/tracker_log.c` (mirrors `ble_auto_walk_log.c` storage lifecycle + CSV quoting):

```c
#include "tracker_log.h"

#include <furi.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>

static const char CSV_HEADER[] =
    "timestamp,category,kind,address,name,best_rssi,event\n";

struct TrackerLog {
    Storage* storage;
    File* file;
};

TrackerLog* tracker_log_open(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TRACKER_LOG_DIR);

    bool existed = (storage_common_stat(storage, TRACKER_LOG_PATH, NULL) == FSE_OK);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, TRACKER_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    TrackerLog* log = malloc(sizeof(TrackerLog));
    if(!log) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }
    log->storage = storage;
    log->file = file;

    if(!existed) storage_file_write(file, CSV_HEADER, sizeof(CSV_HEADER) - 1);
    return log;
}

void tracker_log_close(TrackerLog* log) {
    if(!log) return;
    if(log->file) {
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) furi_record_close(RECORD_STORAGE);
    free(log);
}

// Write the name as a CSV-escaped quoted field (doubles embedded quotes,
// replaces newlines with spaces). Same approach as ble_auto_walk_log.c.
static void write_name_quoted(File* f, const char* s) {
    storage_file_write(f, "\"", 1);
    if(s) {
        const char* run = s;
        const char* p = s;
        while(*p) {
            if(*p == '"') {
                if(p > run) storage_file_write(f, run, p - run);
                storage_file_write(f, "\"\"", 2);
                run = p + 1;
            } else if(*p == '\n' || *p == '\r') {
                if(p > run) storage_file_write(f, run, p - run);
                storage_file_write(f, " ", 1);
                run = p + 1;
            }
            p++;
        }
        if(p > run) storage_file_write(f, run, p - run);
    }
    storage_file_write(f, "\"", 1);
}

void tracker_log_alert(TrackerLog* log, TrackerCategory cat, const TrackerDevice* dev) {
    if(!log || !log->file || !dev) return;

    char buf[128];
    int n = snprintf(
        buf, sizeof(buf),
        "%lu,%s,%s,%02X:%02X:%02X:%02X:%02X:%02X,",
        (unsigned long)furi_get_tick(),
        tracker_category_label(cat),
        ble_tracker_kind_label(dev->kind),
        dev->addr[0], dev->addr[1], dev->addr[2],
        dev->addr[3], dev->addr[4], dev->addr[5]);
    if(n > 0) storage_file_write(log->file, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);

    write_name_quoted(log->file, dev->name);

    n = snprintf(buf, sizeof(buf), ",%d,FOLLOW\n", dev->best_rssi);
    if(n > 0) storage_file_write(log->file, buf, n > (int)sizeof(buf) ? (int)sizeof(buf) : n);
}
```

- [ ] **Step 3: Write the picker scene**

Create `applications/main/ble_spam/scenes/scene_tracker_pick.c`:

```c
#include "../ble_spam_app.h"
#include "../tracker_category.h"

static void tracker_pick_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_tracker_pick_on_enter(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
    for(uint32_t i = 0; i < TrackerCatCount; i++) {
        submenu_add_item(
            app->submenu, tracker_category_label((TrackerCategory)i), i,
            tracker_pick_callback, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_tracker_pick_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event < TrackerCatCount) {
            app->tracker_category = (TrackerCategory)event.event;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerScan);
            consumed = true;
        }
    }
    return consumed;
}

void ble_spam_scene_tracker_pick_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
```

- [ ] **Step 4: Register the picker scene**

Edit `applications/main/ble_spam/scenes/scene_config.h` — add the picker line immediately before the `tracker_scan` line:

```c
ADD_SCENE(ble_spam, tracker_pick, TrackerPick)
ADD_SCENE(ble_spam, tracker_scan, TrackerScan)
```

- [ ] **Step 5: Add the app field**

Edit `applications/main/ble_spam/ble_spam_app.h`:

(a) After `#include "ble_tracker_hal.h"` add:

```c
#include "ble_tracker_hal.h"
#include "tracker_category.h"
```

(b) In `struct BleSpamApp`, in the "BLE Tracker state" block (right after `TrackerDevice tracker_target;`), add:

```c
    TrackerDevice tracker_target;
    TrackerCategory tracker_category;   // chosen in the picker, read by the scan scene
```

- [ ] **Step 6: Route the menu to the picker**

Edit `applications/main/ble_spam/scenes/scene_main.c` — in `ble_spam_scene_main_on_event`, change the Tracker case target:

```c
        case MainMenuIndexBleTracker:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerPick);
            consumed = true;
            break;
```

- [ ] **Step 7: Filter the scan scene by category + log alerts**

Edit `applications/main/ble_spam/scenes/scene_tracker_scan.c`:

(a) Add includes after the existing `#include "../views/tracker_list_view.h"`:

```c
#include "../views/tracker_list_view.h"
#include "../tracker_category.h"
#include "../tracker_log.h"
```

(b) Replace the file-static `s_sweep_mode` line with a log handle:

Remove:
```c
static bool s_sweep_mode;                 // false=Trackers, true=Bug Sweep
```
Add (in its place):
```c
static TrackerLog* s_log;                 // alerts-only SD CSV (NULL if no SD)
```

(c) In `on_enter`, replace the sweep-setup block:

Remove:
```c
    s_sweep_mode = false;
    s_alerted_count = 0;
    s_beep_ticks = 0;
    ble_tracker_hal_set_sweep(false);

    ble_tracker_hal_start_scan();
```
Add (in its place):
```c
    s_alerted_count = 0;
    s_beep_ticks = 0;
    ble_tracker_hal_set_sweep(tracker_category_uses_sweep(app->tracker_category));
    s_log = tracker_log_open();

    ble_tracker_hal_start_scan();
```

(d) Delete the Left/Right toggle branch entirely (it referenced `s_sweep_mode`):

Remove this whole `else if`:
```c
        } else if(event.event == InputKeyLeft || event.event == InputKeyRight) {
            furi_hal_light_blink_stop();
            furi_hal_light_set_rgb_all(0, 0, 0);
            s_sweep_mode = !s_sweep_mode;
            ble_tracker_hal_set_sweep(s_sweep_mode);
            s_alerted_count = 0;
            model->selected = 0;
            model->window_offset = 0;
            model->count = 0;
            view_commit_model(app->view_tracker_scan, true);
            consumed = true;
        }
```
so the chain ends after the `InputKeyOk` branch's closing `}`.

(e) In the Tick handler, replace the copy-into-model block with a category-filtered copy. Change:
```c
        if(count > TRACKER_MAX_DEVICES) count = TRACKER_MAX_DEVICES;
        if(count > 0) memcpy(model->devices, devices, count * sizeof(TrackerDevice));
        model->count = count;
```
to:
```c
        // Filter the HAL list down to the chosen category (all variants of it).
        uint16_t fcount = 0;
        for(uint16_t i = 0; i < count && fcount < TRACKER_MAX_DEVICES; i++) {
            if(tracker_category_matches(app->tracker_category, devices[i].kind)) {
                model->devices[fcount++] = devices[i];
            }
        }
        count = fcount;              // the rest of the tick operates on the filtered set
        model->count = count;
```

(f) Set the header mode flag from the category. Change:
```c
        model->sweep_mode = s_sweep_mode;
```
to:
```c
        model->sweep_mode = tracker_category_uses_sweep(app->tracker_category);
```

(g) Log each new FOLLOW! alert. In the alert loop, add the log call right after `fire_alert();`:
```c
                alerted_add(model->devices[i].addr);
                fire_alert();
                tracker_log_alert(s_log, app->tracker_category, &model->devices[i]);
```

(h) In `on_exit`, close the log. `on_exit` currently starts `UNUSED(context);` — change it to take the app and close the log. Replace:
```c
void ble_spam_scene_tracker_scan_on_exit(void* context) {
    UNUSED(context);
    furi_hal_light_blink_stop();
```
with:
```c
void ble_spam_scene_tracker_scan_on_exit(void* context) {
    UNUSED(context);
    tracker_log_close(s_log);
    s_log = NULL;
    furi_hal_light_blink_stop();
```

- [ ] **Step 8: Build the Cardputer ADV target (via PowerShell)**

Run the build through the PowerShell tool:

```
cmd /c "`"C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_fix.bat`""
```

Verify (do NOT trust exit code alone):
- `build_fix.log` mtime advanced to now, AND its last markers include `SENTINEL_OK` (not `SENTINEL_BUILD_FAIL`).
- The new objects exist:
```
wsl.exe -- bash -lc "ls /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15/build_cardputer_adv/esp-idf/main/CMakeFiles/esp32_fam_app_ble_spam.dir/__/applications/main/ble_spam/{tracker_category,tracker_log,scenes/scene_tracker_pick}.c.obj"
```
Expected: all three `.obj` present. On `SENTINEL_BUILD_FAIL`, read the compiler error in `build_fix.log`, fix, re-run. If the log shows target `esp32c6` or SPIRAM re-enabled, run `clean_build_jul12.bat` (set-target esp32s3) via PowerShell instead, then re-run `build_fix.bat` to merge.

- [ ] **Step 9: Build the StickS3 target (via PowerShell)**

```
cmd /c "`"C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_m5stick_s3.bat`""
```
Verify `build_m5stick_s3.log` mtime advanced + `SENTINEL_OK`.

- [ ] **Step 10: Re-run the host test (regression guard)**

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/tcat tests/test_tracker_category.c applications/main/ble_spam/tracker_category.c && /tmp/tcat"
```
Expected: `ALL TRACKER CATEGORY TESTS PASSED`.

- [ ] **Step 11: Commit**

```
git add applications/main/ble_spam/tracker_log.h applications/main/ble_spam/tracker_log.c applications/main/ble_spam/scenes/scene_tracker_pick.c applications/main/ble_spam/scenes/scene_config.h applications/main/ble_spam/ble_spam_app.h applications/main/ble_spam/scenes/scene_main.c applications/main/ble_spam/scenes/scene_tracker_scan.c
git commit -m "feat(ble_spam): fox-hunt target picker + category filter + alerts-only SD log"
```

---

## Task 3: Direction-finding aid in the geiger (smoothed RSSI + peak-hold + trend)

**Files:**
- Modify: `applications/main/ble_spam/views/tracker_geiger_view.h`
- Modify: `applications/main/ble_spam/views/tracker_geiger_view.c`
- Modify: `applications/main/ble_spam/scenes/scene_tracker_geiger.c`

**Interfaces:**
- Consumes (existing): `TrackerGeigerModel`, the geiger scene tick + timer, `app->tracker_current_rssi` (beep source), the geiger view context = `ViewDispatcher*` (set in `ble_spam_app.c:90`).
- Produces: model fields `rssi_smooth`, `rssi_peak`, `trend`; an OK-reset custom event handled in the scene.

Purely additive to the geiger. No host test (RSSI-smoothing is trivial arithmetic verified on-device); the deliverable is a clean build + the manual smoke test. Uses the same PowerShell build + verify discipline as Task 2 (rebuild both targets, confirm `SENTINEL_OK` + the geiger `.obj` recompiled).

- [ ] **Step 1: Add model fields**

Edit `applications/main/ble_spam/views/tracker_geiger_view.h` — extend `TrackerGeigerModel` (add three fields after `int8_t rssi;`):

```c
typedef struct {
    uint8_t target_addr[6];
    TrackerKind target_kind;
    char target_name[24];
    int8_t rssi;             // raw latest RSSI
    int8_t rssi_smooth;      // EMA of rssi (drives label/bar/beep)
    int8_t rssi_peak;        // decaying peak-hold marker (~1 dB/s)
    int8_t trend;            // +1 rising, -1 falling, 0 steady
    uint32_t last_seen_ms;
    bool stale;
} TrackerGeigerModel;
```

- [ ] **Step 2: Redraw the geiger with smoothed value + peak marker + trend + hint, and forward OK**

Edit `applications/main/ble_spam/views/tracker_geiger_view.c`. Replace the draw callback body (from the "Proximity label" comment through the bargraph block) and the input callback with:

Replace the draw callback's body *after* the header block (i.e. from `// Proximity label (large, centered)` to the end of the function) with:

```c
    // Proximity label from the SMOOTHED rssi (less jitter)
    canvas_set_font(canvas, FontPrimary);
    const char* label = proximity_label(model->rssi_smooth, model->stale);
    canvas_draw_str_aligned(canvas, 64, 25, AlignCenter, AlignCenter, label);

    // Smoothed RSSI value + trend arrow
    canvas_set_font(canvas, FontSecondary);
    char rssi_text[20];
    if(model->stale) {
        snprintf(rssi_text, sizeof(rssi_text), "--- dBm");
    } else {
        const char* arrow = model->trend > 0 ? " ^" : (model->trend < 0 ? " v" : "  ");
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm%s", model->rssi_smooth, arrow);
    }
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, rssi_text);

    // RSSI bargraph with peak-hold marker: -100 dBm = empty, -30 dBm = full
    const uint8_t bar_x = 4;
    const uint8_t bar_y = 46;
    const uint8_t bar_w = 120;
    const uint8_t bar_h = 8;
    elements_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    if(!model->stale) {
        int range = model->rssi_smooth + 100; // -100..-30 -> 0..70
        if(range < 0) range = 0;
        if(range > 70) range = 70;
        uint8_t fill_w = (uint8_t)((range * (bar_w - 2)) / 70);
        if(fill_w > 0) canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);

        int prange = model->rssi_peak + 100;
        if(prange < 0) prange = 0;
        if(prange > 70) prange = 70;
        uint8_t px = (uint8_t)(bar_x + 1 + (prange * (bar_w - 2)) / 70);
        if(px > (uint8_t)(bar_x + bar_w - 1)) px = bar_x + bar_w - 1;
        canvas_draw_line(canvas, px, bar_y - 1, px, bar_y + bar_h);
    }

    // Direction-finding hint
    canvas_draw_str_aligned(
        canvas, 64, 63, AlignCenter, AlignBottom,
        model->stale ? "no signal - move" : "rotate  OK=reset");
}
```

Replace the input callback with one that forwards a short OK press (the view context is the `ViewDispatcher`, set in `ble_spam_app.c`):

```c
static bool tracker_geiger_input_callback(InputEvent* event, void* context) {
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        view_dispatcher_send_custom_event((ViewDispatcher*)context, InputKeyOk);
        return true;
    }
    return false;
}
```

- [ ] **Step 3: Compute smoothing / peak / trend in the scene, handle OK-reset, drive beep from smoothed**

Edit `applications/main/ble_spam/scenes/scene_tracker_geiger.c`.

(a) Add file-statics for the smoothing/peak state, after the `#define BEEP_DURATION_MS 50` line:

```c
static int8_t s_prev_smooth;      // previous smoothed value (for trend)
static bool s_have_prev;
static uint32_t s_peak_decay_ms;  // last time the peak was decayed
```

(b) In `on_enter`, initialize the new model fields + state. After the existing `model->rssi = app->tracker_target.rssi;` line add:

```c
    model->rssi = app->tracker_target.rssi;
    model->rssi_smooth = app->tracker_target.rssi;
    model->rssi_peak = app->tracker_target.rssi;
    model->trend = 0;
```

and after `app->tracker_current_period = 1200;` add:

```c
    s_have_prev = false;
    s_peak_decay_ms = furi_get_tick();
```

(c) In `on_event`, add a Custom-event branch for OK-reset *before* the existing `if(event.type == SceneManagerEventTypeTick)`:

```c
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            TrackerGeigerModel* model = view_get_model(app->view_tracker_geiger);
            model->rssi_peak = model->rssi_smooth;   // clear peak for a fresh sweep
            view_commit_model(app->view_tracker_geiger, true);
            s_peak_decay_ms = furi_get_tick();
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
```

(change the existing `if(event.type == SceneManagerEventTypeTick) {` to the `} else if(...)` shown above — i.e. chain it onto the new Custom branch).

(d) Inside the Tick branch, after `model->stale = stale;` and *before* `view_commit_model(...)`, insert the smoothing/peak/trend computation:

```c
        model->stale = stale;

        // EMA smoothing: smooth = (2*smooth + raw)/3
        int32_t sm = ((int32_t)model->rssi_smooth * 2 + (int32_t)model->rssi) / 3;
        model->rssi_smooth = (int8_t)sm;

        // Trend from the smoothed delta (+/-1 dB threshold)
        if(s_have_prev) {
            int d = (int)model->rssi_smooth - (int)s_prev_smooth;
            model->trend = (d >= 1) ? 1 : (d <= -1 ? -1 : 0);
        } else {
            model->trend = 0;
        }
        s_prev_smooth = model->rssi_smooth;
        s_have_prev = true;

        // Peak-hold: instant rise, ~1 dB/s decay toward the current smoothed value
        if(model->rssi_smooth > model->rssi_peak) {
            model->rssi_peak = model->rssi_smooth;
        } else if((now - s_peak_decay_ms) >= 1000) {
            if(model->rssi_peak > model->rssi_smooth) model->rssi_peak--;
            s_peak_decay_ms = now;
        }

        view_commit_model(app->view_tracker_geiger, true);
```

(This replaces the existing `view_commit_model(app->view_tracker_geiger, true);` that came right after `model->stale = stale;` — do not leave a duplicate commit.)

(e) Drive the beep from the smoothed value. Change the mirror line:

```c
        app->tracker_current_rssi = model->rssi;
```
to:
```c
        app->tracker_current_rssi = model->rssi_smooth;
```

- [ ] **Step 4: Build both targets (PowerShell) + verify**

Build Cardputer ADV and StickS3 exactly as Task 2 Steps 8–9 (PowerShell `cmd /c "…bat"`, verify log mtime + `SENTINEL_OK` + the geiger `.obj` recompiled:
`build_cardputer_adv/.../ble_spam/views/tracker_geiger_view.c.obj` and `.../scenes/scene_tracker_geiger.c.obj`).

- [ ] **Step 5: Commit**

```
git add applications/main/ble_spam/views/tracker_geiger_view.h applications/main/ble_spam/views/tracker_geiger_view.c applications/main/ble_spam/scenes/scene_tracker_geiger.c
git commit -m "feat(ble_spam): RSSI direction-finding aid in tracker geiger (smoothed + peak-hold + trend + OK reset)"
```

---

## Manual on-hardware smoke test (after flashing)

Cardputer (has SD): **Bluetooth → Tracker** shows the 7-item picker. Pick **Meta** → only Meta glasses listed; **Apple** → AirTags/AirPods/iPhone only; **Bug Sweep** → unknowns; Back returns to picker. Leave a target nearby → escalates to FOLLOW! (LED + beep) and a row appears in `/ext/ble/tracker_hunt.csv`. Select a device → geiger. **Direction aid:** hold the device to your chest and rotate — the bar tracks the smoothed RSSI, the peak-marker parks at the strongest heading (decays after ~a second), the ▲/▼ arrow shows warmer/colder, and OK clears the peak for a fresh sweep. StickS3 (no SD): same minus logging, no crash.

---

## Self-Review

**Spec coverage:**
- Picker with 7 categories (success #1,#2) → Task 2 Steps 3,5,6 + Task 1 enum/labels. ✓
- Filtered scan (#3) → Task 2 Step 7(e). ✓
- Alerts + sort preserved (#4) → untouched alert/sort code; filter feeds it. ✓
- Geiger unchanged (#5) → not modified. ✓
- Alerts-only SD log + no-SD no-op (#6) → Task 2 Steps 1,2,7(c,g,h); `tracker_log_open` returns NULL on no SD, all calls guarded. ✓
- Both builds (#7) → Steps 8,9. ✓
- Host tests for the pure matcher → Task 1. ✓

**Placeholder scan:** none — every step has complete code or an exact edit with before/after.

**Type consistency:** `TrackerCategory` + `tracker_category_*` names identical across Task 1 header, picker, scan scene, and log. `BleSpamSceneTrackerPick`/`BleSpamViewSubmenu` are existing/macro-generated. `TrackerListModel.sweep_mode`/`count`/`devices` match `tracker_list_view.h`. `TrackerDevice.addr/name/kind/best_rssi` match `ble_tracker_hal.h`. `s_log` file-static replaces `s_sweep_mode` consistently (added, all `s_sweep_mode` uses removed in Steps 7b,c,d,f). Task 3: `TrackerGeigerModel.rssi_smooth/rssi_peak/trend` added in Step 1 and consumed identically in the view (Step 2) and scene (Step 3); the geiger view context is a `ViewDispatcher*` (confirmed `ble_spam_app.c:90`) so the forwarded OK event is valid.

**Task 3 (direction-finding aid):** Additive to the geiger scene/view — success criterion beyond the spec's original scope, added per user request. Smoothed RSSI + decaying peak-hold + rise/fall trend + OK-to-reset; the proximity beep now tracks the smoothed value. No new files, no host test (trivial arithmetic, verified on-device).
