# WhisperPair Surface Detector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a detector for WhisperPair (CVE-2025-36911) to the `ble_spam` app: scan for Google Fast Pair (0xFE2C) devices, enumerate their GATT attack surface read-only, and report an exposure verdict — no exploit write.

**Architecture:** A new scene + view mirroring the existing Airoha RACE detector (`scene_race_detector.c` / `race_detector_view.c`), driving the same `ble_walk_hal` NimBLE GATT client. A small pure C module (`whisperpair_fp.c/.h`) holds the host-testable advertisement parser and the known-vulnerable name matcher, exactly like `tracker_follow.c/.h`.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), Flipper app framework (scene manager + view dispatcher), NimBLE GATT client via `ble_walk_hal`, host unit tests via `gcc` under WSL (no libcheck). Build via `idf.py` wrapped in `build_fix.bat` (Cardputer ADV) and `build_m5stick_s3.bat` (StickS3).

## Global Constraints

- **Detector-only.** No `ble_walk_hal_write_char` call anywhere in this feature. No SMP pairing, no bond, no Account-Key write. Verdict derives solely from service/characteristic *presence* + a *read* of Model ID.
- **Pure module purity:** `whisperpair_fp.c/.h` uses only `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`, `<string.h>` — no esp/furi includes — so it host-compiles.
- **Git Bash has no host `gcc`** — run host tests through WSL: `wsl.exe -- bash -lc "cd /mnt/c/... && gcc ... && ./out"`.
- **New `.c` files auto-compile:** `application.fam` does not list sources; the build globs the app tree. Adding `.c` files under `applications/main/ble_spam/` is enough — no sources list to edit.
- **UUID byte order:** ESP-IDF stores 128-bit UUIDs little-endian; a 128-bit UUID array is the textual UUID reversed. 16-bit SIG UUIDs arrive as `esp_bt_uuid_t{ .len = ESP_UUID_LEN_16, .uuid.uuid16 = 0x…}` (confirmed in `ble_walk_hal.c` NimBLE→esp conversion).
- **Branch:** `nimble-port`. Commit message trailer lines as used elsewhere in the repo are optional; keep messages scoped to this feature.
- **Build gate:** every firmware change ends with both `build_fix.bat` and `build_m5stick_s3.bat` reaching `Project build complete` / `SENTINEL_OK`.

---

## File Structure

- **New** `applications/main/ble_spam/whisperpair_fp.h` — `FpAdvKind`, `fp_adv_parse()`, `fp_name_is_vuln()`.
- **New** `applications/main/ble_spam/whisperpair_fp.c` — pure implementations + known-vulnerable name table.
- **New** `tests/test_whisperpair_fp.c` — host unit tests for the pure module.
- **New** `applications/main/ble_spam/views/whisperpair_detector_view.h` / `.c` — list view + model (clone of `race_detector_view`).
- **New** `applications/main/ble_spam/scenes/scene_whisperpair_detector.c` — scan/select/probe scene (read-only clone of `scene_race_detector.c`), holds the Fast Pair char UUID arrays + `esp_bt_uuid_t` matchers.
- **Modify** `applications/main/ble_spam/scenes/scene_config.h` — register the scene.
- **Modify** `applications/main/ble_spam/ble_spam_app.h` — view enum + view pointer + `wp_probe_abort`.
- **Modify** `applications/main/ble_spam/ble_spam_app.c` — alloc / add-view / remove-view / free the view.
- **Modify** `applications/main/ble_spam/scenes/scene_main.c` — one `"WhisperPair"` menu item.

---

## Task 1: Pure Fast Pair parser + vuln-name matcher (host-tested)

**Files:**
- Create: `applications/main/ble_spam/whisperpair_fp.h`
- Create: `applications/main/ble_spam/whisperpair_fp.c`
- Test: `tests/test_whisperpair_fp.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `typedef enum { FpAdvNone = 0, FpAdvDiscoverable, FpAdvHasAccounts } FpAdvKind;`
  - `FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out);`
  - `bool fp_name_is_vuln(const char* name);`

- [ ] **Step 1: Write the header**

Create `applications/main/ble_spam/whisperpair_fp.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fast Pair advertisement classification (Google Fast Pair spec):
//   0xFE2C service data, 3-byte payload  -> Model ID present; provider is
//                                           discoverable / in pairing mode.
//   0xFE2C service data, other length    -> Account-Key filter (Bloom); provider
//                                           is non-discoverable, has bonded accounts.
//   no 0xFE2C service data               -> not a Fast Pair advertisement.
typedef enum {
    FpAdvNone = 0,
    FpAdvDiscoverable,
    FpAdvHasAccounts,
} FpAdvKind;

// Walk a BLE advertisement TLV buffer for Service Data - 16-bit UUID (AD type
// 0x16) whose UUID is 0xFE2C. Returns the classification; when the payload is
// exactly the 3-byte Model ID, writes the 24-bit model id (big-endian in the
// advert, i.e. octet 0 is MSB) into *model_id_out (if non-NULL).
// Pure: no esp/furi deps; bounds-checked (no over-read on malformed input).
FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out);

// Case-insensitive substring match of `name` against the known-vulnerable
// device-name table (whisperpair.eu/vulnerable-devices). NULL/empty -> false.
bool fp_name_is_vuln(const char* name);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_whisperpair_fp.c`:

```c
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/whisperpair_fp.h"

static void test_discoverable(void) {
    // AD field: len=0x06, type=0x16 (svc data 16-bit), uuid 2C FE (LE), model AA BB CC
    uint8_t adv[] = {0x06, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC};
    uint32_t model = 0;
    assert(fp_adv_parse(adv, sizeof(adv), &model) == FpAdvDiscoverable);
    assert(model == 0xAABBCCu); // big-endian: octet0 MSB
    printf("test_discoverable OK\n");
}

static void test_has_accounts(void) {
    // 0xFE2C service data, 5-byte payload (flags + 4) -> account-key filter
    uint8_t adv[] = {0x08, 0x16, 0x2C, 0xFE, 0x00, 0x11, 0x22, 0x33, 0x44};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvHasAccounts);
    printf("test_has_accounts OK\n");
}

static void test_none(void) {
    // Flags AD (0x01) then service data for a DIFFERENT uuid (0xFEAA)
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x05, 0x16, 0xAA, 0xFE, 0x01, 0x02};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvNone);
    printf("test_none OK\n");
}

static void test_malformed(void) {
    // field length 0x20 overruns a 4-byte buffer -> no over-read, FpAdvNone
    uint8_t adv[] = {0x20, 0x16, 0x2C, 0xFE};
    assert(fp_adv_parse(adv, sizeof(adv), NULL) == FpAdvNone);
    // zero-length field terminates the walk
    uint8_t adv2[] = {0x00, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC};
    assert(fp_adv_parse(adv2, sizeof(adv2), NULL) == FpAdvNone);
    printf("test_malformed OK\n");
}

static void test_name_match(void) {
    assert(fp_name_is_vuln("Sony WH-1000XM5") == true);
    assert(fp_name_is_vuln("wh-1000xm5") == true);          // case-insensitive
    assert(fp_name_is_vuln("Pixel Buds Pro 2") == true);
    assert(fp_name_is_vuln("My Random Speaker") == false);
    assert(fp_name_is_vuln("") == false);
    assert(fp_name_is_vuln(NULL) == false);
    printf("test_name_match OK\n");
}

int main(void) {
    test_discoverable();
    test_has_accounts();
    test_none();
    test_malformed();
    test_name_match();
    printf("ALL WHISPERPAIR FP TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run (from any dir; uses WSL gcc):

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/wpfp tests/test_whisperpair_fp.c applications/main/ble_spam/whisperpair_fp.c && /tmp/wpfp"
```

Expected: **FAILS to compile/link** — `whisperpair_fp.c` does not exist yet (`No such file or directory`).

- [ ] **Step 4: Write the implementation**

Create `applications/main/ble_spam/whisperpair_fp.c`:

```c
#include "whisperpair_fp.h"

#include <stddef.h>
#include <string.h>

#define FP_SERVICE_UUID16 0xFE2C

FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out) {
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;              // zero-length field: end of data
        if(pos + fld_len >= len) break;      // field overruns buffer: stop, no over-read
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;

        // AD type 0x16 = Service Data - 16-bit UUID: [uuid_lo, uuid_hi, data...]
        if(type == 0x16 && val_len >= 2) {
            uint16_t uuid = (uint16_t)(val[0] | (val[1] << 8));
            if(uuid == FP_SERVICE_UUID16) {
                uint8_t data_len = (uint8_t)(val_len - 2);
                const uint8_t* data = &val[2];
                if(data_len == 3) {
                    if(model_id_out) {
                        *model_id_out = ((uint32_t)data[0] << 16) |
                                        ((uint32_t)data[1] << 8) |
                                        (uint32_t)data[2];
                    }
                    return FpAdvDiscoverable;
                }
                return FpAdvHasAccounts;
            }
        }

        pos += fld_len + 1;
    }
    return FpAdvNone;
}

// Known-vulnerable device names (substring, whisperpair.eu/vulnerable-devices).
// Match is a HINT layered on top of the surface verdict, never the sole signal.
static const char* const FP_VULN_NAMES[] = {
    "WH-1000XM6", "WH-1000XM5", "WH-1000XM4", "WF-1000XM5", "WH-CH720N",
    "Pixel Buds Pro 2", "Nord Buds 3 Pro", "Redmi Buds 5 Pro",
    "MOTIF II", "Liberty 4 NC", "Elite 8 Active", "Tune Beam", "Nothing Ear",
};

static char up(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static bool ci_contains(const char* hay, const char* needle) {
    if(!hay || !needle || !needle[0]) return false;
    for(const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while(*a && *b && up(*a) == up(*b)) {
            a++;
            b++;
        }
        if(!*b) return true;
    }
    return false;
}

bool fp_name_is_vuln(const char* name) {
    if(!name || !name[0]) return false;
    for(size_t i = 0; i < sizeof(FP_VULN_NAMES) / sizeof(FP_VULN_NAMES[0]); i++) {
        if(ci_contains(name, FP_VULN_NAMES[i])) return true;
    }
    return false;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/wpfp tests/test_whisperpair_fp.c applications/main/ble_spam/whisperpair_fp.c && /tmp/wpfp"
```

Expected: five `... OK` lines then `ALL WHISPERPAIR FP TESTS PASSED`, exit 0, **no warnings**.

- [ ] **Step 6: Commit**

```
git add applications/main/ble_spam/whisperpair_fp.h applications/main/ble_spam/whisperpair_fp.c tests/test_whisperpair_fp.c
git commit -m "feat(ble_spam): pure Fast Pair advert parser + vuln-name matcher for WhisperPair detector"
```

(Use `git add` with these explicit paths — the working tree has unrelated staged files; do not `git add -A`.)

---

## Task 2: WhisperPair detector view, scene, and wiring (firmware builds green)

**Files:**
- Create: `applications/main/ble_spam/views/whisperpair_detector_view.h`
- Create: `applications/main/ble_spam/views/whisperpair_detector_view.c`
- Create: `applications/main/ble_spam/scenes/scene_whisperpair_detector.c`
- Modify: `applications/main/ble_spam/scenes/scene_config.h`
- Modify: `applications/main/ble_spam/ble_spam_app.h`
- Modify: `applications/main/ble_spam/ble_spam_app.c`
- Modify: `applications/main/ble_spam/scenes/scene_main.c`

**Interfaces:**
- Consumes (Task 1): `FpAdvKind`, `fp_adv_parse()`, `fp_name_is_vuln()`.
- Consumes (existing `ble_walk_hal.h`): `ble_walk_hal_start/stop`, `ble_walk_hal_start_scan/stop_scan`, `ble_walk_hal_get_devices`, `ble_walk_hal_connect/disconnect`, `ble_walk_hal_discover_services/services_ready/get_services`, `ble_walk_hal_discover_chars/chars_ready/get_chars`, `ble_walk_hal_read_char/read_ready/get_read_value`. Types `BleWalkDevice`, `BleWalkService`, `BleWalkChar`, `esp_bt_uuid_t`, `ESP_UUID_LEN_16/128`.
- Produces:
  - View: `View* whisperpair_detector_view_alloc(void); void whisperpair_detector_view_free(View* view);`
  - Model types `WpStatus`, `WpDevice`, `WpDetectorModel` (in the view header).
  - Scene handlers `ble_spam_scene_whisperpair_detector_on_enter/on_event/on_exit` (declared automatically by `scenes.h` via the `ADD_SCENE` macro once registered).
  - App fields `View* view_whisperpair_detector; volatile bool wp_probe_abort;` and enum `BleSpamViewWhisperpairDetector`.

- [ ] **Step 1: Write the view header**

Create `applications/main/ble_spam/views/whisperpair_detector_view.h`:

```c
#pragma once

#include <gui/view.h>
#include "../ble_compat_defs.h"
#include "../whisperpair_fp.h"

#define WP_DETECTOR_MAX_DEVICES     24
#define WP_DETECTOR_ITEMS_ON_SCREEN 4

typedef enum {
    WpStatusUnknown = 0,
    WpStatusProbing,
    WpStatusNoSurface,    // connected, but no 0xFE2C service / no KBP characteristic
    WpStatusConnectFail,
    WpStatusSurface,      // 0xFE2C + writable Key-Based Pairing characteristic present
    WpStatusLikelyVuln,   // WpStatusSurface AND name matches known-vulnerable list
} WpStatus;

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    char name[24];
    FpAdvKind adv_kind;
    uint32_t model_id;
    bool model_id_valid;
    WpStatus status;
} WpDevice;

typedef struct {
    WpDevice devices[WP_DETECTOR_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    bool probing;
} WpDetectorModel;

View* whisperpair_detector_view_alloc(void);
void whisperpair_detector_view_free(View* view);
```

- [ ] **Step 2: Write the view implementation**

Create `applications/main/ble_spam/views/whisperpair_detector_view.c`:

```c
#include "whisperpair_detector_view.h"
#include "../ble_spam_app.h"
#include <gui/canvas.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <stdio.h>

static char wp_status_glyph(WpStatus s) {
    switch(s) {
    case WpStatusLikelyVuln:  return 'V';
    case WpStatusSurface:     return 'S';
    case WpStatusProbing:     return '.';
    case WpStatusNoSurface:   return '-';
    case WpStatusConnectFail: return 'x';
    case WpStatusUnknown:
    default:                  return '?';
    }
}

static void whisperpair_detector_draw_callback(Canvas* canvas, void* _model) {
    WpDetectorModel* model = _model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    char title[32];
    snprintf(title, sizeof(title), "WhisperPair (%d)", model->count);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 128, 12);

    canvas_set_font(canvas, FontSecondary);

    if(model->count == 0) {
        canvas_draw_str_aligned(
            canvas, 64, 36, AlignCenter, AlignCenter, "Scanning FE2C...");
    }

    for(int i = 0;
        i < WP_DETECTOR_ITEMS_ON_SCREEN && (model->window_offset + i) < model->count;
        i++) {
        uint16_t idx = model->window_offset + i;
        WpDevice* d = &model->devices[idx];

        const char* label = d->name[0] ? d->name : "(unnamed)";
        char advc = d->adv_kind == FpAdvDiscoverable ?
                        'd' :
                        (d->adv_kind == FpAdvHasAccounts ? 'a' : ' ');

        char line[40];
        snprintf(
            line,
            sizeof(line),
            "%c %-9.9s %c %d",
            wp_status_glyph(d->status),
            label,
            advc,
            d->rssi);

        uint8_t y = 22 + i * 11;
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y - 8, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, line);
        if(idx == model->selected) {
            canvas_set_color(canvas, ColorBlack);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    if(model->probing) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Probing...");
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Probe");
    }
}

static bool whisperpair_detector_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyOk || event->key == InputKeyUp ||
           event->key == InputKeyDown) {
            view_dispatcher_send_custom_event(vd, event->key);
            return true;
        }
    }
    return false;
}

View* whisperpair_detector_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WpDetectorModel));
    view_set_draw_callback(view, whisperpair_detector_draw_callback);
    view_set_input_callback(view, whisperpair_detector_input_callback);
    return view;
}

void whisperpair_detector_view_free(View* view) {
    view_free(view);
}
```

- [ ] **Step 3: Write the scene (read-only probe)**

Create `applications/main/ble_spam/scenes/scene_whisperpair_detector.c`:

```c
// WhisperPair (CVE-2025-36911) surface detector: identifies Google Fast Pair
// (0xFE2C) devices and enumerates their GATT attack surface READ-ONLY. It never
// writes the Key-Based Pairing characteristic — it only reports whether that
// surface exists (+ a known-vulnerable model-name hint). No bond, no exploit.
#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../whisperpair_fp.h"
#include "../views/whisperpair_detector_view.h"

#include <string.h>

// Fast Pair GATT characteristics (Google Fast Pair spec). ESP-IDF stores 128-bit
// UUIDs little-endian, so each array is the textual UUID reversed.
// Model ID:          FE2C1233-8366-4814-8EB0-01DE32100BEA
static const uint8_t FP_UUID_MODEL_ID[16] = {
    0xEA, 0x0B, 0x10, 0x32, 0xDE, 0x01, 0xB0, 0x8E,
    0x14, 0x48, 0x66, 0x83, 0x33, 0x12, 0x2C, 0xFE,
};
// Key-based Pairing:  FE2C1234-8366-4814-8EB0-01DE32100BEA  (the WhisperPair surface)
static const uint8_t FP_UUID_KBP[16] = {
    0xEA, 0x0B, 0x10, 0x32, 0xDE, 0x01, 0xB0, 0x8E,
    0x14, 0x48, 0x66, 0x83, 0x34, 0x12, 0x2C, 0xFE,
};

#define FP_SERVICE_UUID16 0xFE2C

static bool uuid_is_fp_service(const esp_bt_uuid_t* u) {
    if(u->len == ESP_UUID_LEN_16) return u->uuid.uuid16 == FP_SERVICE_UUID16;
    if(u->len == ESP_UUID_LEN_128) {
        // SIG base 0000FE2C-0000-1000-8000-00805F9B34FB, little-endian.
        static const uint8_t base[16] = {
            0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x2C, 0xFE, 0x00, 0x00,
        };
        return memcmp(u->uuid.uuid128, base, 16) == 0;
    }
    return false;
}

static bool uuid_is_128(const esp_bt_uuid_t* u, const uint8_t ref[16]) {
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, ref, 16) == 0;
}

static void refresh_device_list(BleSpamApp* app) {
    uint16_t count = 0;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);

    WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);

    for(uint16_t i = 0; i < count; i++) {
        BleWalkDevice* src = &devices[i];

        uint32_t model_id = 0;
        FpAdvKind k = fp_adv_parse(src->adv_data, src->adv_data_len, &model_id);
        if(k == FpAdvNone && src->scan_rsp_len) {
            k = fp_adv_parse(src->scan_rsp_data, src->scan_rsp_len, &model_id);
        }
        if(k == FpAdvNone) continue; // not a Fast Pair advertiser

        bool found = false;
        for(uint16_t j = 0; j < model->count; j++) {
            if(memcmp(model->devices[j].addr, src->addr, sizeof(esp_bd_addr_t)) == 0) {
                model->devices[j].rssi = src->rssi;
                model->devices[j].adv_kind = k;
                if(k == FpAdvDiscoverable && !model->devices[j].model_id_valid) {
                    model->devices[j].model_id = model_id;
                    model->devices[j].model_id_valid = true;
                }
                if(src->name[0]) {
                    strncpy(
                        model->devices[j].name,
                        src->name,
                        sizeof(model->devices[j].name) - 1);
                    model->devices[j].name[sizeof(model->devices[j].name) - 1] = '\0';
                }
                found = true;
                break;
            }
        }
        if(!found && model->count < WP_DETECTOR_MAX_DEVICES) {
            WpDevice* dst = &model->devices[model->count];
            memcpy(dst->addr, src->addr, sizeof(esp_bd_addr_t));
            dst->addr_type = src->addr_type;
            dst->rssi = src->rssi;
            dst->adv_kind = k;
            dst->model_id = model_id;
            dst->model_id_valid = (k == FpAdvDiscoverable);
            strncpy(dst->name, src->name, sizeof(dst->name) - 1);
            dst->name[sizeof(dst->name) - 1] = '\0';
            dst->status = WpStatusUnknown;
            model->count++;
        }
    }

    if(model->count > 0 && model->selected >= model->count) {
        model->selected = model->count - 1;
    }
    view_commit_model(app->view_whisperpair_detector, true);
}

// READ-ONLY probe: connect, confirm the 0xFE2C service + writable KBP
// characteristic (the surface), best-effort READ of Model ID, disconnect.
// There is deliberately NO write to any characteristic here.
static WpStatus probe_device(BleSpamApp* app, WpDevice* target) {
    BleWalkDevice walk_dev = {0};
    memcpy(walk_dev.addr, target->addr, sizeof(esp_bd_addr_t));
    walk_dev.addr_type = target->addr_type;

    app->wp_probe_abort = false;
    if(!ble_walk_hal_connect(&walk_dev, &app->wp_probe_abort)) {
        return WpStatusConnectFail;
    }
    if(!ble_walk_hal_discover_services()) {
        ble_walk_hal_disconnect();
        return WpStatusConnectFail;
    }
    for(int i = 0; i < 60 && !ble_walk_hal_services_ready(); i++) {
        if(app->wp_probe_abort) {
            ble_walk_hal_disconnect();
            return WpStatusConnectFail;
        }
        furi_delay_ms(50);
    }
    if(!ble_walk_hal_services_ready()) {
        ble_walk_hal_disconnect();
        return WpStatusConnectFail;
    }

    uint16_t svc_count = 0;
    BleWalkService* services = ble_walk_hal_get_services(&svc_count);
    BleWalkService* fp = NULL;
    for(uint16_t i = 0; i < svc_count; i++) {
        if(uuid_is_fp_service(&services[i].uuid)) {
            fp = &services[i];
            break;
        }
    }
    if(!fp) {
        ble_walk_hal_disconnect();
        return WpStatusNoSurface;
    }

    bool kbp_present = false;
    uint16_t model_handle = 0;
    if(ble_walk_hal_discover_chars(fp)) {
        for(int i = 0; i < 60 && !ble_walk_hal_chars_ready(); i++) {
            if(app->wp_probe_abort) {
                ble_walk_hal_disconnect();
                return WpStatusConnectFail;
            }
            furi_delay_ms(50);
        }
        if(ble_walk_hal_chars_ready()) {
            uint16_t ch_count = 0;
            BleWalkChar* chars = ble_walk_hal_get_chars(&ch_count);
            for(uint16_t i = 0; i < ch_count; i++) {
                if(uuid_is_128(&chars[i].uuid, FP_UUID_KBP)) kbp_present = true;
                if(uuid_is_128(&chars[i].uuid, FP_UUID_MODEL_ID))
                    model_handle = chars[i].handle;
            }
        }
    }

    // Best-effort READ of Model ID for display (never changes the verdict).
    if(kbp_present && model_handle && ble_walk_hal_read_char(model_handle)) {
        for(int i = 0; i < 40 && !ble_walk_hal_read_ready(); i++) {
            if(app->wp_probe_abort) break;
            furi_delay_ms(50);
        }
        if(ble_walk_hal_read_ready()) {
            uint16_t vlen = 0;
            uint8_t* v = ble_walk_hal_get_read_value(&vlen);
            if(v && vlen >= 3) {
                target->model_id =
                    ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | (uint32_t)v[2];
                target->model_id_valid = true;
            }
        }
    }

    ble_walk_hal_disconnect();

    if(!kbp_present) return WpStatusNoSurface;
    return fp_name_is_vuln(target->name) ? WpStatusLikelyVuln : WpStatusSurface;
}

void ble_spam_scene_whisperpair_detector_on_enter(void* context) {
    BleSpamApp* app = context;

    WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);
    memset(model, 0, sizeof(WpDetectorModel));
    view_commit_model(app->view_whisperpair_detector, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWhisperpairDetector);

    if(!ble_walk_hal_start()) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    ble_walk_hal_start_scan();
}

bool ble_spam_scene_whisperpair_detector_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);

        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_whisperpair_detector, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + WP_DETECTOR_ITEMS_ON_SCREEN)
                    model->window_offset =
                        model->selected - WP_DETECTOR_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_whisperpair_detector, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count == 0 || model->selected >= model->count) {
                view_commit_model(app->view_whisperpair_detector, false);
                return true;
            }

            WpDevice target_copy = model->devices[model->selected];
            uint16_t selected_idx = model->selected;

            model->devices[selected_idx].status = WpStatusProbing;
            model->probing = true;
            view_commit_model(app->view_whisperpair_detector, true);

            ble_walk_hal_stop_scan();

            WpStatus result = probe_device(app, &target_copy);

            WpDetectorModel* m2 = view_get_model(app->view_whisperpair_detector);
            for(uint16_t i = 0; i < m2->count; i++) {
                if(memcmp(m2->devices[i].addr, target_copy.addr, sizeof(esp_bd_addr_t)) ==
                   0) {
                    m2->devices[i].status = result;
                    m2->devices[i].model_id = target_copy.model_id;
                    m2->devices[i].model_id_valid = target_copy.model_id_valid;
                    break;
                }
            }
            m2->probing = false;
            view_commit_model(app->view_whisperpair_detector, true);

            ble_walk_hal_start_scan();
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        app->wp_probe_abort = true;
        ble_walk_hal_disconnect();
        ble_walk_hal_stop_scan();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);
        if(!model->probing) {
            view_commit_model(app->view_whisperpair_detector, false);
            refresh_device_list(app);
        } else {
            view_commit_model(app->view_whisperpair_detector, false);
        }
    }

    return consumed;
}

void ble_spam_scene_whisperpair_detector_on_exit(void* context) {
    BleSpamApp* app = context;
    app->wp_probe_abort = true;
    ble_walk_hal_disconnect();
    ble_walk_hal_stop_scan();
    ble_walk_hal_stop();
}
```

- [ ] **Step 4: Register the scene**

Edit `applications/main/ble_spam/scenes/scene_config.h` — add one line after the `race_detector` line (last line of the file):

```c
ADD_SCENE(ble_spam, race_detector, RaceDetector)
ADD_SCENE(ble_spam, whisperpair_detector, WhisperpairDetector)
```

- [ ] **Step 5: Add the view id, view pointer, and abort flag to the app**

Edit `applications/main/ble_spam/ble_spam_app.h`:

In the `BleSpamViewId` enum, add after `BleSpamViewRaceDetector,`:

```c
    BleSpamViewRaceDetector,
    BleSpamViewWhisperpairDetector,
} BleSpamViewId;
```

In `struct BleSpamApp`, add after the RACE fields (`View* view_race_detector; volatile bool race_probe_abort;`):

```c
    // Airoha RACE Detector state (CVE-2025-20700)
    View* view_race_detector;
    volatile bool race_probe_abort;

    // WhisperPair Detector state (CVE-2025-36911, Fast Pair)
    View* view_whisperpair_detector;
    volatile bool wp_probe_abort;
```

- [ ] **Step 6: Alloc / add / remove / free the view in the app**

Edit `applications/main/ble_spam/ble_spam_app.c`:

(a) After `#include "views/race_detector_view.h"` add:

```c
#include "views/race_detector_view.h"
#include "views/whisperpair_detector_view.h"
```

(b) After the RACE alloc block (the four lines ending `app->race_probe_abort = false;`) add:

```c
    // WhisperPair Detector view (CVE-2025-36911, Fast Pair)
    app->view_whisperpair_detector = whisperpair_detector_view_alloc();
    view_set_context(app->view_whisperpair_detector, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher,
        BleSpamViewWhisperpairDetector,
        app->view_whisperpair_detector);
    app->wp_probe_abort = false;
```

(c) After `view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRaceDetector);` add:

```c
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRaceDetector);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWhisperpairDetector);
```

(d) After `race_detector_view_free(app->view_race_detector);` add:

```c
    race_detector_view_free(app->view_race_detector);
    whisperpair_detector_view_free(app->view_whisperpair_detector);
```

- [ ] **Step 7: Add the menu item**

Edit `applications/main/ble_spam/scenes/scene_main.c`:

(a) In `enum MainMenuIndex`, add after `MainMenuIndexBleRaceDetector,`:

```c
    MainMenuIndexBleRaceDetector,
    MainMenuIndexBleWhisperPair,
};
```

(b) In `ble_spam_scene_main_on_enter`, after the `"Airoha RACE"` `submenu_add_item(...)` call, add:

```c
    submenu_add_item(
        app->submenu,
        "WhisperPair",
        MainMenuIndexBleWhisperPair,
        main_menu_callback,
        app);
```

(c) In `ble_spam_scene_main_on_event`, after the `case MainMenuIndexBleRaceDetector:` block, add:

```c
        case MainMenuIndexBleWhisperPair:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWhisperpairDetector);
            consumed = true;
            break;
```

- [ ] **Step 8: Build the Cardputer ADV target**

Run:

```
cmd.exe /c "cd /d C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15 && build_fix.bat"
```

Then check the log tail:

```
wsl.exe -- bash -lc "tail -n 8 '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15/build_fix.log'"
```

Expected: `Project build complete` and a final `SENTINEL_OK` line. If it ends in `SENTINEL_BUILD_FAIL`, read the compiler error above it in `build_fix.log`, fix, and re-run.

- [ ] **Step 9: Build the StickS3 target**

Run:

```
cmd.exe /c "cd /d C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15 && build_m5stick_s3.bat"
```

Then check:

```
wsl.exe -- bash -lc "tail -n 8 '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15/build_m5stick_s3.log'"
```

Expected: `Project build complete` / `SENTINEL_OK`.

- [ ] **Step 10: Re-run the host test (regression guard)**

Task 2 did not touch the pure module, but confirm it still links and passes:

```
wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -o /tmp/wpfp tests/test_whisperpair_fp.c applications/main/ble_spam/whisperpair_fp.c && /tmp/wpfp"
```

Expected: `ALL WHISPERPAIR FP TESTS PASSED`.

- [ ] **Step 11: Commit**

```
git add applications/main/ble_spam/views/whisperpair_detector_view.h applications/main/ble_spam/views/whisperpair_detector_view.c applications/main/ble_spam/scenes/scene_whisperpair_detector.c applications/main/ble_spam/scenes/scene_config.h applications/main/ble_spam/ble_spam_app.h applications/main/ble_spam/ble_spam_app.c applications/main/ble_spam/scenes/scene_main.c
git commit -m "feat(ble_spam): WhisperPair (CVE-2025-36911) Fast Pair surface detector scene + view"
```

(Explicit paths again — do not `git add -A`; unrelated files are staged in the tree.)

---

## Manual on-hardware smoke test (after flashing, optional but recommended)

Flash the Cardputer ADV merged bin, open **Bluetooth → WhisperPair**:
1. With Fast Pair earbuds or an Android phone advertising Fast Pair nearby, the list populates; entries tagged `d` (discoverable) or `a` (has-accounts).
2. Select one, press OK: verdict resolves to `S` (surface) or `V` (likely-vuln for a listed model); a non-Fast-Pair device (if one slips in) → `-`.
3. Press Back and re-enter a few times: scan stops, connection closes, no hang or stuck radio.

---

## Self-Review

**Spec coverage:**
- Success criterion 1 (list FP devices, tag disc/accounts) → Task 2 Step 3 `refresh_device_list` + Step 2 view row tag. ✓
- Criterion 2 (connect, confirm service + KBP char, read Model ID, disconnect, no write) → Task 2 Step 3 `probe_device` (read-only). ✓
- Criterion 3 (verdict labels) → `WpStatus` enum + `wp_status_glyph`. ✓ (`NoSurface` replaces the spec's `NotFastPair`/`FP-unverified` split with a single "no surface" state; `Surface` = the spec's "FP (unverified)", `LikelyVuln` = spec "LIKELY VULN". Semantics preserved.)
- Criterion 4 (menu entry beside Airoha RACE) → Task 2 Step 7. ✓
- Criterion 5 (clean teardown) → `on_exit` + Back handler. ✓
- Criterion 6 (both targets build) → Task 2 Steps 8–9. ✓
- Pure parser host tests → Task 1. ✓
- "No write_char" constraint → enforced in `probe_device`; verifiable by grep. ✓

**Placeholder scan:** no TBD/TODO/"handle errors"/"similar to"; every code step shows complete code. ✓

**Type consistency:** `WpStatus`/`WpDevice`/`WpDetectorModel` defined in the view header (Task 2 Step 1) and used identically in the scene (Step 3) and view (Step 2). `fp_adv_parse`/`fp_name_is_vuln`/`FpAdvKind` signatures match Task 1's header. `BleSpamViewWhisperpairDetector` and `BleSpamSceneWhisperpairDetector` (macro-generated from `ADD_SCENE(..., whisperpair_detector, WhisperpairDetector)`) used consistently. `view_whisperpair_detector` / `wp_probe_abort` field names consistent across app.h/app.c/scene. ✓

**Note — spec deviation:** the spec proposed a header-only `whisperpair_fp.h` with the pure logic inline; the plan uses a `.c/.h` split matching the `tracker_follow.c/.h` precedent, which the spec's own Testing section already assumed (`tests/test_whisperpair_fp.c` linking the module). The `esp_bt_uuid_t` UUID matchers live in the scene (not the pure module), keeping `whisperpair_fp.c` esp-free — same division as `scene_race_detector.c` holding `RACE_UUID_*`.
