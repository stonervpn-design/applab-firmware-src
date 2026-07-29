# Tracker / Bug Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the existing BLE "Tracker" scanner into a counter-surveillance tool: add time-based "following" detection (with a FOLLOW! alert: banner + RGB LED + beep) and a Bug Sweep mode that lists all nearby BLE devices, reusing the existing scan HAL and geiger locator.

**Architecture:** A new pure module (`tracker_follow`) provides the host-tested follow-state classifier and a BLE local-name parser. The tracker HAL gains a `first_seen_ms` field (reset when a device returns from stale, so dwell = continuous presence) and a sweep mode that also records unknown BLE devices. The scan scene computes follow-state, sorts (FOLLOW! first, then signal), toggles sweep with Left/Right, and fires a debounced alert. The list view renders the badge/dwell/mode. The geiger locator is unchanged.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), Flipper app framework (scene manager + view dispatcher), BLE GAP (`esp_gap_ble_api`), `furi_hal_light` (WS2812), `furi_hal_speaker` (ES8311). Host unit tests via `gcc` (no libcheck).

## Global Constraints

- Target board `m5stack_cardputer_adv`. Firmware build: `build_ci.bat` (Windows). Success tail: `Project build complete` + `=== BUILD OK ===`; regenerated `Flipper-cardputer_adv-merged.bin`. Invoke from Git Bash SYNCHRONOUSLY (not backgrounded):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"` (timeout 600000 ms). `build_*.log` is gitignored. `build_cardputer_adv/` is validly configured → incremental; do NOT delete it.
- New `tracker_follow.c/.h` MUST be pure C (`<stdint.h>`, `<stdbool.h>`, `<string.h>` only) — no esp/furi — so it host-compiles. Git Bash has no host `gcc`; use WSL: `wsl.exe -- bash -lc "cd /mnt/c/.../ && gcc ... && ./a.out"`.
- Follow thresholds (exact, `#define` in tracker_follow.h): `FOLLOW_STALE_MS 30000`, `FOLLOW_PRESENT_MS 60000`, `FOLLOW_SUSPECT_MS 180000`, `FOLLOW_FOLLOWING_MS 300000`.
- Follow labels (exact): FollowNew→`"NEW"`, FollowPresent→`"SEEN"`, FollowSuspect→`"SUSPECT"`, FollowFollowing→`"FOLLOW!"`.
- `dwell = last_seen_ms - first_seen_ms` (uint32 modular). A device with `now - last_seen >= FOLLOW_STALE_MS` is inactive → `FollowNew` regardless of dwell.
- `TRACKER_MAX_DEVICES` becomes 40 (was 24).
- Sweep default OFF (known-trackers-only = current behavior). Toggling clears the device table.
- This repo is a git repo on `master`; commit per task.

---

### Task 1: Pure follow classifier + BLE name parser + host tests

**Files:**
- Create: `applications/main/ble_spam/tracker_follow.h`
- Create: `applications/main/ble_spam/tracker_follow.c`
- Test: `tests/test_tracker_follow.c`

**Interfaces:**
- Produces (used by Tasks 2, 3, 4):
  - `FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms);`
  - `const char* tracker_follow_label(FollowState s);`
  - `bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz);`
  - enum `FollowState { FollowNew, FollowPresent, FollowSuspect, FollowFollowing };`

- [ ] **Step 1: Write the header**

Create `applications/main/ble_spam/tracker_follow.h`:

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FollowNew,
    FollowPresent,
    FollowSuspect,
    FollowFollowing,
} FollowState;

#define FOLLOW_STALE_MS      30000u
#define FOLLOW_PRESENT_MS    60000u
#define FOLLOW_SUSPECT_MS    180000u
#define FOLLOW_FOLLOWING_MS  300000u

/* Escalate by continuous-presence dwell (last_seen - first_seen), but only while
 * ACTIVE (seen within FOLLOW_STALE_MS). A stale device returns FollowNew. All
 * args are furi_get_tick() milliseconds; arithmetic is uint32 modular (wrap-safe). */
FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms);

/* "NEW" / "SEEN" / "SUSPECT" / "FOLLOW!" */
const char* tracker_follow_label(FollowState s);

/* Extract a BLE local name (AD type 0x09 complete or 0x08 shortened) from an
 * advertisement TLV buffer into out (NUL-terminated, truncated to out_sz-1).
 * Returns true only if a non-empty name was found. Pure — no esp/furi deps. */
bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_tracker_follow.c`:

```c
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/tracker_follow.h"

static void test_thresholds(void) {
    uint32_t t = 1000000;
    assert(tracker_follow_state(t, t + 0,      t + 0)      == FollowNew);
    assert(tracker_follow_state(t, t + 59999,  t + 59999)  == FollowNew);
    assert(tracker_follow_state(t, t + 60000,  t + 60000)  == FollowPresent);
    assert(tracker_follow_state(t, t + 179999, t + 179999) == FollowPresent);
    assert(tracker_follow_state(t, t + 180000, t + 180000) == FollowSuspect);
    assert(tracker_follow_state(t, t + 299999, t + 299999) == FollowSuspect);
    assert(tracker_follow_state(t, t + 300000, t + 300000) == FollowFollowing);
    printf("test_thresholds OK\n");
}

static void test_stale(void) {
    uint32_t t = 1000000;
    uint32_t last = t + 600000;               /* 10 min dwell */
    assert(tracker_follow_state(t, last, last + 30000) == FollowNew);        /* stale */
    assert(tracker_follow_state(t, last, last + 29999) == FollowFollowing);  /* active */
    printf("test_stale OK\n");
}

static void test_wrap(void) {
    uint32_t first = 0xFFFFFF00u;
    uint32_t last = first + 300000u;          /* wraps past 2^32 */
    assert(tracker_follow_state(first, last, last) == FollowFollowing);
    printf("test_wrap OK\n");
}

static void test_labels(void) {
    assert(strcmp(tracker_follow_label(FollowNew), "NEW") == 0);
    assert(strcmp(tracker_follow_label(FollowPresent), "SEEN") == 0);
    assert(strcmp(tracker_follow_label(FollowSuspect), "SUSPECT") == 0);
    assert(strcmp(tracker_follow_label(FollowFollowing), "FOLLOW!") == 0);
    printf("test_labels OK\n");
}

static void test_name(void) {
    char out[24];
    uint8_t a1[] = {0x06, 0x09, 'H','e','l','l','o'};       /* complete name */
    assert(ble_adv_extract_local_name(a1, sizeof(a1), out, sizeof(out)) && !strcmp(out, "Hello"));
    uint8_t a2[] = {0x04, 0x08, 'A','B','C'};               /* shortened name */
    assert(ble_adv_extract_local_name(a2, sizeof(a2), out, sizeof(out)) && !strcmp(out, "ABC"));
    uint8_t a3[] = {0x02, 0x01, 0x06, 0x03, 0xFF, 0x4C, 0x00}; /* flags + mfg, no name */
    assert(!ble_adv_extract_local_name(a3, sizeof(a3), out, sizeof(out)));
    uint8_t a4[] = {0x09, 0x09, 'A','B','C','D','E','F','G','H'}; /* name longer than out */
    char small[4];
    assert(ble_adv_extract_local_name(a4, sizeof(a4), small, sizeof(small)) && !strcmp(small, "ABC"));
    printf("test_name OK\n");
}

int main(void) {
    test_thresholds(); test_stale(); test_wrap(); test_labels(); test_name();
    printf("ALL TRACKER FOLLOW TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

From Git Bash, project root (WSL gcc — Git Bash has no host compiler):

```bash
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/ttf tests/test_tracker_follow.c applications/main/ble_spam/tracker_follow.c && /tmp/ttf"
```

Expected: **FAILS** to compile/link — `tracker_follow.c` does not exist yet.

- [ ] **Step 4: Implement the module**

Create `applications/main/ble_spam/tracker_follow.c`:

```c
#include "tracker_follow.h"
#include <string.h>

FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms) {
    if((uint32_t)(now_ms - last_seen_ms) >= FOLLOW_STALE_MS) return FollowNew;
    uint32_t dwell = (uint32_t)(last_seen_ms - first_seen_ms);
    if(dwell >= FOLLOW_FOLLOWING_MS) return FollowFollowing;
    if(dwell >= FOLLOW_SUSPECT_MS) return FollowSuspect;
    if(dwell >= FOLLOW_PRESENT_MS) return FollowPresent;
    return FollowNew;
}

const char* tracker_follow_label(FollowState s) {
    switch(s) {
    case FollowFollowing: return "FOLLOW!";
    case FollowSuspect:   return "SUSPECT";
    case FollowPresent:   return "SEEN";
    default:              return "NEW";
    }
}

bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz) {
    if(!out || out_sz == 0) return false;
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= len) break;            /* field would overrun buffer */
        uint8_t type = buf[pos + 1];
        if(type == 0x09 || type == 0x08) {         /* complete / shortened local name */
            uint8_t nlen = fld_len - 1;
            if(nlen > out_sz - 1) nlen = out_sz - 1;
            memcpy(out, &buf[pos + 2], nlen);
            out[nlen] = '\0';
            return nlen > 0;
        }
        pos += fld_len + 1;
    }
    return false;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/ttf tests/test_tracker_follow.c applications/main/ble_spam/tracker_follow.c && /tmp/ttf"
```

Expected: each `... OK` line then `ALL TRACKER FOLLOW TESTS PASSED`, exit 0, no warnings.

- [ ] **Step 6: Commit**

```bash
git add applications/main/ble_spam/tracker_follow.h applications/main/ble_spam/tracker_follow.c tests/test_tracker_follow.c
git commit -m "feat(ble): add host-tested tracker follow-state classifier + BLE name parser"
```

---

### Task 2: HAL — first_seen + return-from-stale reset, sweep mode, unknown capture

**Files:**
- Modify: `applications/main/ble_spam/ble_tracker_hal.h`
- Modify: `applications/main/ble_spam/ble_tracker_hal.c`

**Interfaces:**
- Consumes: `ble_adv_extract_local_name` (Task 1).
- Produces (used by Tasks 3, 4):
  - `TrackerDevice` gains `uint32_t first_seen_ms;`
  - `void ble_tracker_hal_set_sweep(bool enable);`
  - `TRACKER_MAX_DEVICES` is 40.

- [ ] **Step 1: Header changes**

In `applications/main/ble_spam/ble_tracker_hal.h`:
1. Change `#define TRACKER_MAX_DEVICES 24` → `#define TRACKER_MAX_DEVICES 40`.
2. Add `uint32_t first_seen_ms;` to the `TrackerDevice` struct (next to `last_seen_ms`).
3. Add the prototype near the other functions:

```c
/* Bug-Sweep mode: when enabled, devices that match no known tracker brand are
 * ALSO recorded (kind = TrackerKindUnknown, labeled by BLE local name or
 * "mfg 0xXXXX"). Toggling clears the device table. Default disabled. */
void ble_tracker_hal_set_sweep(bool enable);
```

- [ ] **Step 2: Add the sweep flag + label helper (top of ble_tracker_hal.c)**

Add the include and a file-static flag near the top of `ble_tracker_hal.c` (after the existing `static ... s_scanning`):

```c
#include "tracker_follow.h"   /* ble_adv_extract_local_name */

static volatile bool s_sweep = false;
```

Add a helper (above `tracker_gap_event_handler`) that derives a label for an unknown device — a local name if present in either buffer, else the manufacturer company id, else empty:

```c
static void sweep_label(
    const uint8_t* adv, uint8_t adv_len, uint8_t scan_rsp_len,
    char* out, uint8_t out_sz) {
    if(ble_adv_extract_local_name(adv, adv_len, out, out_sz)) return;
    if(scan_rsp_len > 0 && ble_adv_extract_local_name(adv + adv_len, scan_rsp_len, out, out_sz))
        return;
    /* No name — try manufacturer company id from AD type 0xFF in the adv buffer. */
    uint8_t pos = 0;
    while(pos + 1 < adv_len) {
        uint8_t fld_len = adv[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= adv_len) break;
        if(adv[pos + 1] == 0xFF && fld_len >= 3) {
            uint16_t cid = adv[pos + 2] | (adv[pos + 3] << 8);
            snprintf(out, out_sz, "mfg 0x%04X", cid);
            return;
        }
        pos += fld_len + 1;
    }
    out[0] = '\0';
}
```

(`snprintf` needs `#include <stdio.h>` — add it to the includes if not present.)

- [ ] **Step 3: Sweep capture in the GAP result handler**

In `tracker_gap_event_handler`, `ESP_GAP_BLE_SCAN_RESULT_EVT`, replace the early-out
`if(!match) break;` so that in sweep mode an unmatched advert is recorded as Unknown:

```c
            if(!match) {
                if(!s_sweep) break;
                kind = TrackerKindUnknown;
                sweep_label(
                    param->scan_rst.ble_adv,
                    param->scan_rst.adv_data_len,
                    param->scan_rst.scan_rsp_len,
                    name,
                    sizeof(name));
            }
```

- [ ] **Step 4: first_seen + return-from-stale reset in dedup/create**

Still in the result handler, update the found/new branches. In the **found** branch, before updating `last_seen_ms`, add the stale-reset; in the **new** branch, set `first_seen_ms`:

Found branch — add at the top of the `if(found >= 0)` block:

```c
                TrackerDevice* d = &s_devices[found];
                if((uint32_t)(now - d->last_seen_ms) >= FOLLOW_STALE_MS) {
                    d->first_seen_ms = now;   /* returned from stale → fresh presence */
                }
```

(then the existing rssi/best_rssi/last_seen/name/kind updates follow unchanged).

New branch — add alongside the other field initializers in the `else if(s_device_count < TRACKER_MAX_DEVICES)` block:

```c
                d->first_seen_ms = now;
```

`FOLLOW_STALE_MS` comes from `tracker_follow.h` (already included in Step 2).

- [ ] **Step 5: Implement `ble_tracker_hal_set_sweep`**

Add to `ble_tracker_hal.c` (near the other public functions):

```c
void ble_tracker_hal_set_sweep(bool enable) {
    s_sweep = enable;
    /* Clear the table so the list reflects the active mode. */
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
}
```

- [ ] **Step 6: Build**

Run the standard build (`build_tracker2.log`). Expected `=== BUILD OK ===`.

- [ ] **Step 7: Commit**

```bash
git add applications/main/ble_spam/ble_tracker_hal.h applications/main/ble_spam/ble_tracker_hal.c
git commit -m "feat(ble): tracker HAL first_seen + return-from-stale reset, sweep mode capture"
```

---

### Task 3: List view — model fields, Left/Right forwarding, follow badge + mode header

**Files:**
- Modify: `applications/main/ble_spam/views/tracker_list_view.h`
- Modify: `applications/main/ble_spam/views/tracker_list_view.c`

**Interfaces:**
- Consumes: `tracker_follow_state`, `tracker_follow_label` (Task 1); `TrackerDevice.first_seen_ms` (Task 2).
- Produces (used by Task 4): `TrackerListModel` gains `uint32_t now_ms;` and `bool sweep_mode;`.

- [ ] **Step 1: Model fields**

In `tracker_list_view.h`, add to `TrackerListModel`:

```c
    uint32_t now_ms;    // furi_get_tick() at publish, for follow-state calc
    bool sweep_mode;    // header label: false=Trackers, true=Bug Sweep
```

- [ ] **Step 2: Forward Left/Right from the input callback**

In `tracker_list_view.c`, `tracker_list_input_callback`, extend the forwarded keys so the scene receives Left/Right (for the mode toggle):

```c
        if(event->key == InputKeyOk || event->key == InputKeyUp ||
           event->key == InputKeyDown || event->key == InputKeyLeft ||
           event->key == InputKeyRight) {
            view_dispatcher_send_custom_event(vd, event->key);
        }
```

- [ ] **Step 3: Draw follow badge, dwell, mode header, unknown labels**

In `tracker_list_draw_callback`, include the classifier and update the header + rows. Add at the top of the file with the other includes:

```c
#include "../tracker_follow.h"
```

Replace the title line to show the mode:

```c
    char title[32];
    snprintf(title, sizeof(title), "%s (%d)", model->sweep_mode ? "Bug Sweep" : "Tracker", model->count);
    canvas_draw_str(canvas, 2, 10, title);
```

In the per-row loop, compute the follow-state and render a compact badge + dwell.
Replace the label pick + `snprintf(line, ...)` body with:

```c
        // Label: model/local name if present, else kind label, else MAC tail.
        char maclabel[8];
        const char* label;
        if(d->name[0]) {
            label = d->name;
        } else if(d->kind != TrackerKindUnknown) {
            label = ble_tracker_kind_label(d->kind);
        } else {
            snprintf(maclabel, sizeof(maclabel), "%02X%02X", d->addr[4], d->addr[5]);
            label = maclabel;
        }

        FollowState fs = tracker_follow_state(d->first_seen_ms, d->last_seen_ms, model->now_ms);
        uint32_t dwell_s = (uint32_t)(d->last_seen_ms - d->first_seen_ms) / 1000u;

        char line[40];
        snprintf(
            line, sizeof(line), "%-9.9s %-7s %lus %d",
            label, tracker_follow_label(fs), (unsigned long)dwell_s, d->rssi);
```

(The selection-highlight box and `canvas_draw_str(canvas, 2, y, line)` around this stay
as they are. `%lus` prints dwell seconds; keep the RSSI at the end.)

- [ ] **Step 4: Build**

Run the standard build (`build_tracker3.log`). Expected `=== BUILD OK ===`.

- [ ] **Step 5: Commit**

```bash
git add applications/main/ble_spam/views/tracker_list_view.h applications/main/ble_spam/views/tracker_list_view.c
git commit -m "feat(ble): tracker list view shows follow badge, dwell, mode header"
```

---

### Task 4: Scan scene — mode toggle, sort, follow-state publish, FOLLOW! alert

**Files:**
- Modify: `applications/main/ble_spam/scenes/scene_tracker_scan.c`

**Interfaces:**
- Consumes: `ble_tracker_hal_set_sweep` (Task 2); `TrackerListModel.now_ms`/`sweep_mode`, Left/Right custom events (Task 3); `tracker_follow_state` (Task 1); `furi_hal_light_*`, `furi_hal_speaker_*`.
- Produces: the fully wired feature (no new interfaces).

- [ ] **Step 1: Add includes + scene statics**

At the top of `scene_tracker_scan.c` add:

```c
#include "../tracker_follow.h"
#include <furi_hal_light.h>
#include <furi_hal_resources.h>   /* LightRed enum */
#include <furi_hal_speaker.h>
#include <input/input.h>

static bool s_sweep_mode;                 // false=Trackers, true=Bug Sweep
static uint8_t s_alerted[TRACKER_MAX_DEVICES][6];  // MACs already alerted (FOLLOW!)
static uint8_t s_alerted_count;
static uint8_t s_beep_ticks;              // countdown to stop the alert beep
```

- [ ] **Step 2: Reset statics + sweep in on_enter**

In `ble_spam_scene_tracker_scan_on_enter`, after the existing model reset, add:

```c
    s_sweep_mode = false;
    s_alerted_count = 0;
    s_beep_ticks = 0;
    ble_tracker_hal_set_sweep(false);
```

(place this before `ble_tracker_hal_start_scan();`).

- [ ] **Step 3: Helpers — alerted-set membership + the alert itself**

Add above `ble_spam_scene_tracker_scan_on_event`:

```c
static bool alerted_contains(const uint8_t* mac) {
    for(int i = 0; i < s_alerted_count; i++)
        if(memcmp(s_alerted[i], mac, 6) == 0) return true;
    return false;
}

static void alerted_add(const uint8_t* mac) {
    if(s_alerted_count < TRACKER_MAX_DEVICES) memcpy(s_alerted[s_alerted_count++], mac, 6);
}

// Rebuild the alerted set to only MACs still FOLLOWING this tick, so a device
// that leaves (goes stale) and later returns can re-alert.
static void alerted_prune(TrackerDevice* devs, uint16_t count, uint32_t now) {
    uint8_t kept = 0;
    for(int i = 0; i < s_alerted_count; i++) {
        for(int j = 0; j < count; j++) {
            if(memcmp(s_alerted[i], devs[j].addr, 6) == 0) {
                if(tracker_follow_state(devs[j].first_seen_ms, devs[j].last_seen_ms, now) ==
                   FollowFollowing) {
                    if(kept != i) memcpy(s_alerted[kept], s_alerted[i], 6);
                    kept++;
                }
                break;
            }
        }
    }
    s_alerted_count = kept;
}

static void fire_alert(void) {
    furi_hal_light_blink_start(LightRed, 0xFF, 200, 400); // non-blocking hardware blink
    if(furi_hal_speaker_acquire(50)) {
        furi_hal_speaker_start(880.0f, 0.5f);
        s_beep_ticks = 2;                                 // stopped in the tick handler
    }
}
```

- [ ] **Step 4: Mode toggle on Left/Right**

In the `SceneManagerEventTypeCustom` block of `ble_spam_scene_tracker_scan_on_event`,
add handling for Left/Right (which the view now forwards). Toggle the mode, clear the
HAL table for the new mode, and reset the alert set:

```c
        else if(event.event == InputKeyLeft || event.event == InputKeyRight) {
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

- [ ] **Step 5: Sort + follow publish + alert in the Tick handler**

Replace the body of the `SceneManagerEventTypeTick` branch with a version that sorts
(FOLLOW! first, then best_rssi desc), publishes `now_ms`/`sweep_mode`, and fires the
debounced alert:

```c
    } else if(event.type == SceneManagerEventTypeTick) {
        uint16_t count;
        TrackerDevice* devices = ble_tracker_hal_get_devices(&count);
        uint32_t now = furi_get_tick();

        // Copy into a local array we can sort without disturbing the HAL table.
        TrackerListModel* model = view_get_model(app->view_tracker_scan);
        if(count > TRACKER_MAX_DEVICES) count = TRACKER_MAX_DEVICES;
        if(count > 0) memcpy(model->devices, devices, count * sizeof(TrackerDevice));
        model->count = count;

        // Insertion sort: FOLLOW! first, then higher best_rssi.
        for(int i = 1; i < count; i++) {
            TrackerDevice key = model->devices[i];
            bool key_follow = tracker_follow_state(key.first_seen_ms, key.last_seen_ms, now) ==
                              FollowFollowing;
            int j = i - 1;
            while(j >= 0) {
                bool jf = tracker_follow_state(
                              model->devices[j].first_seen_ms, model->devices[j].last_seen_ms,
                              now) == FollowFollowing;
                bool swap = (key_follow && !jf) ||
                            (key_follow == jf && key.best_rssi > model->devices[j].best_rssi);
                if(!swap) break;
                model->devices[j + 1] = model->devices[j];
                j--;
            }
            model->devices[j + 1] = key;
        }

        model->now_ms = now;
        model->sweep_mode = s_sweep_mode;
        if(model->selected >= count && count > 0) model->selected = count - 1;
        view_commit_model(app->view_tracker_scan, true);

        // Debounced FOLLOW! alert.
        alerted_prune(model->devices, count, now);
        for(int i = 0; i < count; i++) {
            if(tracker_follow_state(model->devices[i].first_seen_ms,
                                    model->devices[i].last_seen_ms, now) == FollowFollowing &&
               !alerted_contains(model->devices[i].addr)) {
                alerted_add(model->devices[i].addr);
                fire_alert();
            }
        }

        // Stop the alert beep after its countdown.
        if(s_beep_ticks > 0 && --s_beep_ticks == 0) {
            if(furi_hal_speaker_is_mine()) {
                furi_hal_speaker_stop();
                furi_hal_speaker_release();
            }
        }
    }
```

Note the OK handler already copies `model->devices[model->selected]` into
`app->tracker_target` — since the model is now sorted, OK correctly targets the
visually-selected (sorted) device. No change needed there.

- [ ] **Step 6: Clean up LED/beeper on exit**

In `ble_spam_scene_tracker_scan_on_exit`, add before/after the existing
`ble_tracker_hal_stop_scan();`:

```c
    furi_hal_light_blink_stop();
    furi_hal_light_set_rgb_all(0, 0, 0);
    if(furi_hal_speaker_is_mine()) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
```

Also stop the blink + beeper in the `SceneManagerEventTypeBack` branch is not needed
separately (Back triggers on_exit), but ensure on_exit runs the cleanup above.

- [ ] **Step 7: Build**

Run the standard build (`build_tracker4.log`). Expected `=== BUILD OK ===` and a
regenerated `Flipper-cardputer_adv-merged.bin`. Confirm no undefined references to
`furi_hal_light_blink_start`/`furi_hal_light_set_rgb_all`/`tracker_follow_state`.

- [ ] **Step 8: Commit**

```bash
git add applications/main/ble_spam/scenes/scene_tracker_scan.c
git commit -m "feat(ble): tracker mode toggle, follow-first sort, debounced FOLLOW! alert"
```

---

### Task 5: On-hardware validation

**Files:** none (manual verification).

- [ ] **Step 1: Flash**

`python -m esptool --chip esp32s3 --port COM12 -b 460800 write_flash 0x0 Flipper-cardputer_adv-merged.bin`

- [ ] **Step 2: Following detection + alert**

BLE app → **Tracker**. Leave a phone/AirTag nearby. Expect the row's badge to climb
`NEW → SEEN → SUSPECT → FOLLOW!` over ~5 min; at FOLLOW! the RGB LED blinks red and a
short beep sounds once. Walk the device away for >30 s → it goes stale; bring it back
→ dwell restarts (fresh presence), and it can alert again later.

- [ ] **Step 3: Bug Sweep mode**

Press Left/Right → header shows "Bug Sweep"; many BLE devices appear (unknowns labeled
by local name or `mfg 0xXXXX`), sorted with FOLLOW!/strongest first. Left/Right back →
"Tracker" (known-only). List clears on each toggle.

- [ ] **Step 4: Locate + lifecycle**

Select any device → OK → geiger locator beeps faster as you approach it. Back out and
re-enter, toggle repeatedly: no stuck LED/beeper, scan stops cleanly, other BLE
features (Walk/Spam) still work.

- [ ] **Step 5: Record result**

```bash
git commit --allow-empty -m "test(ble): Tracker/Bug Sweep validated on Cardputer ADV hardware"
```

---

## Self-Review Notes

- **Spec §3.1 (HAL first_seen + return-from-stale + sweep + unknown capture + 40 cap):** Task 2. ✓
- **Spec §3.2 (pure classifier + name parser + host tests):** Task 1. ✓
- **Spec §3.3 (mode toggle, sort, follow publish, alert):** Task 4. ✓
- **Spec §3.4 (list view model + draw badge/dwell/header/unknown label):** Task 3 (+ Left/Right forwarding, which §3.3 requires and the view owns). ✓
- **Spec §3.5 (geiger unchanged):** no task — confirmed no change needed; OK handler untouched. ✓
- **Spec §3.6 (menu unchanged):** no task. ✓
- **Spec §4 (host tests + hardware):** Task 1 (host) + Task 5 (hardware). ✓
- **Spec §5 risks:** debounce via `s_alerted` set + prune (risk 3); speaker acquire-guard + on_exit release (risk 2); sort surfaces strongest under the 40 cap (risk 1); `set_sweep` only flips a flag + clears table, no stack re-init (risk 4). ✓
- **Deviation from spec §3.3/§6:** `sweep_mode` and alert bookkeeping are scene-local statics (matching the codebase's scene-static idiom), NOT new `BleSpamApp` fields — simpler and local to the one scene that uses them. The view learns the mode via `TrackerListModel.sweep_mode` each tick.
- **Type consistency:** `FollowState`/`tracker_follow_state`/`tracker_follow_label`/`ble_adv_extract_local_name` defined in Task 1 and consumed unchanged in Tasks 2-4; `TrackerListModel.now_ms`/`sweep_mode` defined in Task 3 and set in Task 4; `first_seen_ms` defined in Task 2 and read in Tasks 3-4.
