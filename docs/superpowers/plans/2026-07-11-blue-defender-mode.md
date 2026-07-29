# Blue-Team "Defender" Mode — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkboxes (`- [ ]`): write failing test → run it (fails) → implement → run (passes) → build → commit.
> **Model policy:** opus/haiku only (Sonnet trips the cyber-safety gate — do not use it for any step here).

**Spec:** `docs/superpowers/specs/2026-07-11-blue-defender-mode-design.md`

**Goal:** Stand up the passive, receive-only "Defender" blue-team app (`applications/main/defender/`) and prove the architecture end-to-end by shipping its three shared building blocks (alert bus, SD logger, RSSI locator) plus its self-contained WiFi promiscuous tap and the four highest-value detectors: WiFi deauth/disassoc flood, BLE unwanted-tracker, BLE-spam flood, and Sub-GHz jamming. Every detector's heuristic is a **pure, host-testable** function; every scene leaves its radio cleanly stopped. Phase 2 (eviltwin, beaconflood, karma, fingerprint, nRF24 spectrum, Remote-ID + odid_parser, integrated mode, purple harnesses) is deferred to a titles-only outline at the end.

**Architecture:**
- Defender owns a **self-contained promiscuous tap** (`defender_wifi_tap.c`) built on the *existing* `wlan_hal_*` promiscuous entry points (RX-only). It does **not** refactor `wlan_app` and does **not** depend on any future `wifi_raw` extraction. The tap copies **headers only** into small `static volatile` fixed rings (drop-on-full, exactly like `wlan_client_scanner.c`), restoring radio state on scene exit.
- Each detector = a helper module split into (a) a **pure eval** (deps-free `.c` or `static inline` header, gcc/WSL-testable like `tracker_follow.c` / `pmkid_frames`) and (b) a thin scene/view that feeds it drained records and renders it.
- Three shared, this-app-owned components built in the foundation phase: `defender_alert` (categorized bus + per-(cat,offender) debounce + LED/beep/feed ring), `defender_logger` (CSV to `/ext/apps_data/defender/`, pure row formatting; pcap fns are Phase-1 stubs), `defender_locator` (+view; generalizes `tracker_geiger`; pure RSSI→bar-level + beep-cadence mapping).
- Radios are mutually exclusive on this no-PSRAM S3 (see **Global Constraints → radio discipline**).

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper firmware app under `applications/main/`. Host unit tests are plain C compiled with WSL `gcc` (Git Bash has no host compiler). Board: `m5stack_cardputer_adv` (no PSRAM; CC1101 + nRF24 present).

## Global Constraints

- **Firmware build** (synchronous, never backgrounded, timeout 600000 ms):
  ```
  MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"
  ```
  Success = log contains `Project build complete` **and** `=== BUILD OK ===`, and `EXIT=0`. Do **not** delete `build_cardputer_adv/`. `build_*.log` is gitignored.
- **Host test** (run from Git Bash, project root — uses WSL gcc):
  ```
  wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/dt tests/<test>.c <pure-source(s)> && /tmp/dt"
  ```
  Pure eval modules and `static inline` headers **must not** include `furi.h`, `esp_*`, or `gui/*` — only `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stddef.h>`, `<stdio.h>`, `<math.h>`. This is what lets the whole `.c` (or the header alone) compile on the host.
- **Radio discipline (bake into every scene):** BT and WiFi share the antenna and are mutually exclusive; `wlan_hal_release_bt()` tears down the BLE controller and needs a reboot to run BLE again. Therefore each Phase-1 scene self-contains its radio and does the minimum: WiFi scenes call `wlan_hal_release_bt()` then `wlan_hal_start()` on enter and `wlan_hal_set_promiscuous(false, NULL)` + `wlan_hal_stop()` on exit; BLE scenes call `ble_tracker_hal_start_scan()` on enter / `ble_tracker_hal_stop_scan()` on exit; Sub-GHz scenes acquire/release the CC1101 RSSI reader. **Do not mix WiFi and BLE in one scene in Phase 1** (integrated mode is Phase 2). See resolved ambiguity #1.
- Repo is not a git repo per the harness env, BUT prior plans commit per task; if `git` is available run the commits, otherwise skip the commit step and note it. Commit **per task**.
- The app source is auto-globbed: the Flipper `App(...)` manifest has no `sources=`, so every `.c` under `applications/main/defender/` (including `scenes/` and `views/`) is compiled. Just create the files.

---

## Task 1 — App skeleton + registration (compile + menu gate; no host test)

**Files:**
- Create `applications/main/defender/application.fam`
- Create `applications/main/defender/defender_app.h`
- Create `applications/main/defender/defender_app_i.h`
- Create `applications/main/defender/defender_app.c`
- Create `applications/main/defender/defender_custom_event.h`
- Create `applications/main/defender/scenes/defender_scene.h`
- Create `applications/main/defender/scenes/defender_scene.c`
- Create `applications/main/defender/scenes/defender_scene_config.h`
- Create `applications/main/defender/scenes/defender_scene_start.c`
- Modify `fam_config.py` (add `"defender"` to `APPS`)

**Interfaces produced:** `int32_t defender_app(void* p)` entry point; `struct DefenderApp`; `DefenderScene*` / `DefenderView` / `DefenderCustomEvent` enums.

- [ ] **Step 1: Manifest.** Create `application.fam` (mirror `wlan_app/application.fam`; use a built-in menu icon — **no `icon.png`**, because this port has no Pillow to compile PNGs; see resolved ambiguity #4):
  ```python
  App(
      appid="defender",
      name="Defender",
      apptype=FlipperAppType.MENUEXTERNAL,
      entry_point="defender_app",
      stack_size=8 * 1024,
      icon="A_Plugins_14",
      order=68,
      fap_libs=["assets"],
  )
  ```

- [ ] **Step 2: Register.** In `fam_config.py`, add `"defender",` to the `APPS` list immediately after `"ble_spam",` (line ~50). Do **not** add it to `_boards_without_subghz` / `_boards_without_nrf24` (those strip whole apps; Cardputer-ADV has both radios).

- [ ] **Step 3: Custom events + scene ids.** Create `defender_custom_event.h`:
  ```c
  #pragma once
  typedef enum {
      DefenderEvtDeauth = 100,
      DefenderEvtBleTracker,
      DefenderEvtBleSpam,
      DefenderEvtJammer,
      DefenderEvtLocatorTest,
  } DefenderCustomEvent;
  ```
  Create `scenes/defender_scene_config.h` (only Start exists in T1; later tasks append lines):
  ```c
  ADD_SCENE(defender, start, Start)
  ```
  Create `scenes/defender_scene.h` and `scenes/defender_scene.c` **by copying** `gpio/scenes/gpio_scene.h` and `gpio_scene.c` verbatim and replacing every `gpio`/`Gpio` token with `defender`/`Defender` (the X-macro handler-array boilerplate; `.scene_num = DefenderSceneNum`).

- [ ] **Step 4: App header + internal header.** `defender_app.h`:
  ```c
  #pragma once
  typedef struct DefenderApp DefenderApp;
  ```
  `defender_app_i.h`:
  ```c
  #pragma once
  #include "defender_app.h"
  #include "defender_custom_event.h"
  #include "scenes/defender_scene.h"
  #include <gui/gui.h>
  #include <gui/view_dispatcher.h>
  #include <gui/scene_manager.h>
  #include <gui/modules/submenu.h>
  #include <notification/notification_messages.h>

  typedef enum {
      DefenderViewSubmenu,
      // DefenderViewLocator   (T4)
      // DefenderViewRateMeter (T6)
      // DefenderViewWaterfall (T9)
  } DefenderView;

  struct DefenderApp {
      Gui* gui;
      NotificationApp* notifications;
      ViewDispatcher* view_dispatcher;
      SceneManager* scene_manager;
      Submenu* submenu;
      // Shared components + detector state are added in T2..T9.
  };
  ```

- [ ] **Step 5: App alloc/free/entry.** `defender_app.c` — copy the callback + alloc/free shape from `gpio_app.c` (custom/back/tick callbacks; `view_dispatcher_set_tick_event_callback(..., 100)`), reduced to: open `RECORD_GUI` + `RECORD_NOTIFICATION`; `view_dispatcher_alloc`; `scene_manager_alloc(&defender_scene_handlers, app)`; `submenu_alloc()` added as `DefenderViewSubmenu`; `view_dispatcher_attach_to_gui(..., ViewDispatcherTypeFullscreen)`; `scene_manager_next_scene(app->scene_manager, DefenderSceneStart)`. `_free` tears down in reverse (remove view, `submenu_free`, `view_dispatcher_free`, `scene_manager_free`, close records). Entry:
  ```c
  int32_t defender_app(void* p) {
      UNUSED(p);
      DefenderApp* app = defender_app_alloc();
      view_dispatcher_run(app->view_dispatcher);
      defender_app_free(app);
      return 0;
  }
  ```

- [ ] **Step 6: Start scene.** `scenes/defender_scene_start.c` — `on_enter`: `submenu_reset`; header "Defender"; add items with an enter-callback that fires the matching custom event:
  - "WiFi Deauth"  → `DefenderEvtDeauth`
  - "BLE Tracker"  → `DefenderEvtBleTracker`
  - "BLE Spam"     → `DefenderEvtBleSpam`
  - "SubGHz Jammer"→ `DefenderEvtJammer`
  - "Locator Test" → `DefenderEvtLocatorTest`

  `view_dispatcher_switch_to_view(app->view_dispatcher, DefenderViewSubmenu)`. `on_event`: for T1, `if(event.type == SceneManagerEventTypeCustom) { consumed = true; }` with **no scene pushes yet** — each later task adds its `case`/branch here (see those tasks). `on_exit`: `submenu_reset`. (The items are visible but inert until their scenes land — this is the intended "empty start submenu".)

- [ ] **Step 7: Build** → `build_t1.log`. Expect `=== BUILD OK ===`, EXIT=0, and "Defender" appears in the Tools menu (order 68, right after WiFi/Bluetooth at 67).

- [ ] **Step 8: Commit** `feat(defender): app skeleton + start submenu + registration`.

---

## Task 2 — `defender_alert` (categorized bus + debounce + LED/beep/feed)

**Files:**
- Create `applications/main/defender/defender_alert.h` (pure: enums, structs, `static inline` debounce, prototypes)
- Create `applications/main/defender/defender_alert.c` (firmware: LED/beep/feed ring)
- Create `tests/test_defender_alert.c`
- Modify `defender_app_i.h` (embed `DefenderAlertBus alert;`), `defender_app.c` (init/attach)

**Interfaces produced:**
```c
// defender_alert.h  (pure — includes only <stdint.h> <stdbool.h> <string.h>)
typedef enum {
    DefenderCatWifiDeauth, DefenderCatWifiRogueAp, DefenderCatWifiBeaconFlood,
    DefenderCatWifiKarma,  DefenderCatWifiHwFp,    DefenderCatBleTracker,
    DefenderCatBleSpam,    DefenderCatSubghzJam,   DefenderCatRf24Occ,
    DefenderCatDrone,      DefenderCatCount,
} DefenderCategory;

typedef struct {
    DefenderCategory cat;
    uint8_t  severity;     // 0..2 (info/warn/critical)
    uint8_t  offender[6];  // BSSID / BLE MAC; all-zero if n/a
    int8_t   rssi;
    uint32_t ts_ms;
    char     msg[48];
} DefenderAlert;

#define DEFENDER_DEBOUNCE_MAX 16
#define DEFENDER_ALERT_RING   16
#define DEFENDER_ALERT_STALE_MS 8000u

typedef struct { uint8_t cat; uint8_t offender[6]; uint32_t last_fire_ms; bool used; }
    DefenderDebounceEntry;
typedef struct { DefenderDebounceEntry e[DEFENDER_DEBOUNCE_MAX]; } DefenderDebounceSet;

// PURE, host-tested: returns true iff (cat,offender) should fire now — i.e. no live
// entry, OR the matching entry is stale (now - last_fire_ms >= stale_ms → re-arm).
// On a true return it records/updates the entry with now_ms. Never over-runs the set
// (LRU-evicts the oldest entry when full).
static inline bool defender_debounce_check(
    DefenderDebounceSet* s, uint8_t cat, const uint8_t off[6], uint32_t now_ms, uint32_t stale_ms);

static inline const char* defender_cat_label(DefenderCategory c); // "WIFI/DEAUTH", ...

// firmware (defender_alert.c)
typedef struct {
    DefenderAlert     ring[DEFENDER_ALERT_RING];
    uint8_t           head, count;
    DefenderDebounceSet debounce;
    uint8_t           beep_ticks;
    void*             notifications;   // NotificationApp*
    void*             logger;          // DefenderLogger* (wired in T3; NULL until then)
} DefenderAlertBus;

void defender_alert_init(DefenderAlertBus* bus, void* notifications);
void defender_alert_raise(DefenderAlertBus* bus, const DefenderAlert* a); // debounce→LED+beep+ring+log
void defender_alert_tick(DefenderAlertBus* bus);   // stop the beep after countdown
uint8_t defender_alert_feed(DefenderAlertBus* bus, const DefenderAlert** out); // newest-first view
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_alert.c` (include `../applications/main/defender/defender_alert.h`):
  - `test_labels`: `strcmp(defender_cat_label(DefenderCatWifiDeauth), "WIFI/DEAUTH")==0`; `...BleTracker → "BLE/TRACKER"`; `...SubghzJam → "SUBGHZ/JAM"`.
  - `test_debounce_first_fires`: fresh set, `defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000)` → **true**.
  - `test_debounce_suppresses`: immediately again same (cat,A) at t=1500 → **false**.
  - `test_debounce_rearm_on_stale`: same (cat,A) at t=1000+8000=9000 → **true** (>= stale re-arms).
  - `test_debounce_distinct_offender`: (cat,A) fired at 1000; (cat,B) at 1500 → **true** (different offender).
  - `test_debounce_distinct_cat`: (DeauthCat,A) fired; (BleSpamCat,A) → **true**.
  - `test_debounce_saturation`: fill all 16 entries with distinct offenders at t=1000, then a 17th distinct offender at t=1100 → **true** (evicts oldest, no over-run); re-checking the evicted first offender → **true** (it was dropped).
  (`A={1,2,3,4,5,6}`, `B={1,2,3,4,5,7}`.) `main()` runs all, prints `ALL DEFENDER ALERT TESTS PASSED`.

- [ ] **Step 2: Run test → FAILS** (header missing).
  `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/dt tests/test_defender_alert.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_alert.h`.** Pure header. `defender_cat_label`: `static const char* L[DefenderCatCount]` with `{"WIFI/DEAUTH","WIFI/ROGUE_AP","WIFI/BEACON_FLOOD","WIFI/KARMA","WIFI/HW_FP","BLE/TRACKER","BLE/SPAM","SUBGHZ/JAM","RF24/OCC","DRONE"}`, bounds-checked. `defender_debounce_check`: linear scan for `used && cat==cat && memcmp(offender)==0`; if found and `(uint32_t)(now-last) < stale_ms` → return false; if found stale → update `last_fire_ms=now`, return true; if not found → take first unused slot, else evict slot with smallest `last_fire_ms`; set cat/offender/last/used, return true. Declare (not define) the firmware prototypes.

- [ ] **Step 4: Run test → PASSES.**

- [ ] **Step 5: Implement `defender_alert.c`** (firmware; includes `furi.h`, `furi_hal_light.h`, `furi_hal_speaker.h`, `notification/notification_messages.h`, `defender_alert.h`, and `defender_logger.h` guarded as below). `defender_alert_init`: zero the bus, store `notifications`, `logger=NULL`. `defender_alert_raise`: `if(!defender_debounce_check(&bus->debounce, a->cat, a->offender, a->ts_ms, DEFENDER_ALERT_STALE_MS)) return;` then (a) push a copy into `ring` (newest at `head`, wrap, cap `count`), (b) `furi_hal_light_blink_start(LightRed, 0xFF, 200, 400);` (mirror `scene_tracker_scan.c`'s `fire_alert`), (c) one debounced beep: `if(!furi_hal_speaker_is_mine() && furi_hal_speaker_acquire(50)) { furi_hal_speaker_start(880.0f, 0.5f); bus->beep_ticks = 2; }`, (d) `if(bus->logger) defender_logger_log_alert(bus->logger, a);` — **guard the include/call so this file still links in T2 before T3 exists**: forward-declare `void defender_logger_log_alert(void*, const DefenderAlert*);` locally in T2 and replace with the real header include in T3. `defender_alert_tick`: decrement `beep_ticks`; at 0 `furi_hal_speaker_stop(); if(furi_hal_speaker_is_mine()) furi_hal_speaker_release();`. `defender_alert_feed`: expose newest-first pointer walk for UI.

- [ ] **Step 6: Wire into app.** In `defender_app_i.h` add `DefenderAlertBus alert;` to the struct. In `defender_app.c` alloc, after opening `RECORD_NOTIFICATION`, call `defender_alert_init(&app->alert, app->notifications);` and in the tick callback add `defender_alert_tick(&app->alert);` (so the beep stops even between scenes). In `_free`, before closing notifications, `furi_hal_light_blink_stop();` (safety net).

- [ ] **Step 7: Build** → `build_t2.log`. Expect BUILD OK.

- [ ] **Step 8: Commit** `feat(defender): alert bus with per-(cat,offender) debounce + LED/beep`.

---

## Task 3 — `defender_logger` (CSV event log; pcap stubs)

**Files:**
- Create `applications/main/defender/defender_logger_fmt.h` (pure: `defender_logger_format_row`)
- Create `applications/main/defender/defender_logger.h` (firmware API)
- Create `applications/main/defender/defender_logger.c` (firmware: SD open/write/close; pcap stubs)
- Create `tests/test_defender_logger.c`
- Modify `defender_app_i.h`/`defender_app.c` (own `DefenderLogger* logger;`, wire into `alert.logger`); replace the T2 forward-decl in `defender_alert.c` with the real include.

**Interfaces produced:**
```c
// defender_logger_fmt.h  (pure — includes "defender_alert.h" only)
// Writes exactly one CSV row (with trailing '\n') for `a` into out[0..out_sz).
// Columns: ts_ms,category,severity,offender_mac,rssi,message
// offender_mac = "AA:BB:CC:DD:EE:FF" or "" if all-zero. `message` is CSV-escaped:
// if it contains ',' '"' or '\n' it is wrapped in double-quotes and inner '"' doubled.
// Returns the number of bytes written (excluding NUL), 0 if it would overflow.
size_t defender_logger_format_row(char* out, size_t out_sz, const DefenderAlert* a);

// defender_logger.h  (firmware — includes storage)
typedef struct DefenderLogger DefenderLogger;
DefenderLogger* defender_logger_open(void);   // /ext/apps_data/defender/events-<ts>.csv, header row; NULL on SD fail
void defender_logger_close(DefenderLogger*);
void defender_logger_log_alert(DefenderLogger*, const DefenderAlert*); // format_row → append; bounded flush
// Phase-1 STUBS (implemented in Phase 2):
bool defender_logger_pcap_begin(DefenderLogger*, const char* tag, uint32_t max_packets);
void defender_logger_pcap_write(DefenderLogger*, const uint8_t* frame, uint16_t len, uint32_t ts_us);
void defender_logger_pcap_end(DefenderLogger*);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_logger.c` (include `../applications/main/defender/defender_logger_fmt.h`). Helper builds a `DefenderAlert`. Cases:
  - `test_plain`: cat=`DefenderCatWifiDeauth`, sev=2, offender=`{0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}`, rssi=-42, ts=1234567, msg="Deauth flood ch6 25/s" → row equals `"1234567,WIFI/DEAUTH,2,AA:BB:CC:DD:EE:FF,-42,Deauth flood ch6 25/s\n"`.
  - `test_no_offender`: offender all-zero, msg="RF jam 433.92 +21dB", cat=`DefenderCatSubghzJam`, sev=2, rssi=-30, ts=9 → `"9,SUBGHZ/JAM,2,,-30,RF jam 433.92 +21dB\n"`.
  - `test_escape_comma`: msg="a,b" → the message field equals `"a,b"` (quoted); assert the row contains `,"a,b"\n`.
  - `test_escape_quote`: msg=`He said "hi"` → message field equals `"He said ""hi"""`.
  - `test_overflow`: `out_sz=8` with the plain alert → returns 0 and does not write past the buffer (fill buffer with 0x7F sentinels beyond index 8 and assert untouched).
  `main()` prints `ALL DEFENDER LOGGER TESTS PASSED`.

- [ ] **Step 2: Run → FAILS** (header missing).
  `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/dt tests/test_defender_logger.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_logger_fmt.h`.** Build the row into a local scratch first, measure, then copy iff it fits `out_sz` (leave room for NUL); else return 0. Use `snprintf` for the numeric/mac prefix; MAC printed only if any byte non-zero. Escape the message per the rule (scan for `,`/`"`/`\n`; if none, emit raw; else emit `"` … inner `"`→`""` … `"`). Append `'\n'`.

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Implement `defender_logger.c`** (includes `furi.h`, `storage/storage.h`, `defender_logger.h`, `defender_logger_fmt.h`). `open`: `furi_record_open(RECORD_STORAGE)`; `storage_common_mkdir(storage, "/ext/apps_data/defender")`; build a timestamped path from `furi_hal_rtc_get_datetime` (`events-YYYYMMDD-HHMMSS.csv`); `storage_file_open(..., FSAM_WRITE, FSOM_CREATE_ALWAYS)`; write header `ts_ms,category,severity,offender_mac,rssi,message\n`; on any failure free + `return NULL` (logging becomes a no-op). Keep a `DefenderLogger { Storage*; File*; uint8_t rows_since_flush; }`. `log_alert`: `char row[96]; size_t n = defender_logger_format_row(row, sizeof(row), a); if(n) storage_file_write(f, row, n);` then flush every 8 rows (`if(++rows_since_flush >= 8){ storage_file_sync(f); rows_since_flush=0; }`). `close`: sync, close, `furi_record_close(RECORD_STORAGE)`, free. The three `pcap_*` functions: empty bodies with `UNUSED(...)` and a `// Phase 2` comment.

- [ ] **Step 6: Wire.** In `defender_alert.c` replace the T2 local forward-decl with `#include "defender_logger.h"`. In `defender_app_i.h` add `DefenderLogger* logger;`. In `defender_app.c` alloc: `app->logger = defender_logger_open();` (after `defender_alert_init`) then `app->alert.logger = app->logger;`. In `_free`: `if(app->logger) defender_logger_close(app->logger);`.

- [ ] **Step 7: Build** → `build_t3.log`. Expect BUILD OK.

- [ ] **Step 8: Commit** `feat(defender): SD CSV event logger with escaped rows (pcap stubbed)`.

---

## Task 4 — `defender_locator` (shared RSSI locator + view + scene)

**Files:**
- Create `applications/main/defender/defender_locator_map.h` (pure mapping)
- Create `applications/main/defender/defender_locator.h` / `.c` (firmware controller)
- Create `applications/main/defender/views/defender_locator_view.h` / `.c`
- Create `applications/main/defender/scenes/scene_locator.c`
- Create `tests/test_defender_locator.c`
- Modify `defender_scene_config.h`, `defender_app_i.h`, `defender_app.c`, `defender_scene_start.c`

**Interfaces produced:**
```c
// defender_locator_map.h  (pure)
#define LOCATOR_BAR_MAX 10
// 0..LOCATOR_BAR_MAX; rssi<=floor→0, rssi>=ceil→MAX, linear between.
static inline uint8_t defender_locator_bar_level(int8_t rssi, int8_t floor, int8_t ceil);
// Beep period ms: stale→0 (muted); ceil→80 (fast); floor→1200 (slow); linear between.
static inline uint32_t defender_locator_beep_period_ms(int8_t rssi, bool stale, int8_t floor, int8_t ceil);

// defender_locator.h  (firmware; generalizes tracker_geiger)
typedef struct DefenderLocator DefenderLocator;
DefenderLocator* defender_locator_alloc(void);
void  defender_locator_free(DefenderLocator*);
void  defender_locator_set_bounds(DefenderLocator*, int8_t rssi_floor, int8_t rssi_ceil);
void  defender_locator_set_label(DefenderLocator*, const char* label);
void  defender_locator_push(DefenderLocator*, int8_t rssi, bool stale); // one fresh sample
View* defender_locator_get_view(DefenderLocator*);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_locator.c` (include `../applications/main/defender/defender_locator_map.h`). Bounds `floor=-90, ceil=-40`:
  - bar: `-90 → 0`; `-40 → 10`; `-100 → 0` (clamp low); `-30 → 10` (clamp high); `-65 → 5` (midpoint).
  - beep: `defender_locator_beep_period_ms(-50,false,-90,-40)` in `[80,1200]`; `(-40,false,..)==80`; `(-90,false,..)==1200`; `(-65,false,..)` ≈ `1200-(1120*25/50)` = `640`; `(rssi, true, ..)==0` (stale mutes) for any rssi.
  `main()` prints `ALL DEFENDER LOCATOR TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_locator.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_locator_map.h`** per the formulas above (integer math; guard `ceil>floor`; clamp).

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Implement the view** `views/defender_locator_view.{h,c}` — model `{ char label[28]; int8_t rssi; bool stale; int8_t floor; int8_t ceil; }`. Draw callback: **adapt `tracker_geiger_view.c`** — top line = `label`; centered proximity word from a helper `proximity_label(bar_level)` (`>=9 "RIGHT HERE"`, `>=7 "HOT"`, `>=4 "WARM"`, `>=1 "COOL"`, else `"COLD"`, `stale→"NO SIGNAL"`); `"%d dBm"` / `"--- dBm"`; bottom bargraph filled to `defender_locator_bar_level(rssi,floor,ceil) * (bar_w-2) / LOCATOR_BAR_MAX`. `..._view_alloc/free` like the geiger.

- [ ] **Step 6: Implement the controller** `defender_locator.c` — `DefenderLocator { View* view; int8_t floor, ceil; int8_t ema_rssi; bool have; }`. `alloc` defaults `floor=-90, ceil=-40`. `set_bounds`/`set_label` update the view model. `push(rssi, stale)`: EMA `ema = have ? (ema*7 + rssi*3)/10 : rssi` (alpha 0.3, matches `wlan_client_scanner` smoothing); write `rssi=ema, stale` into the model + commit. The **beep** is driven by the scene's `FuriTimer` reading `defender_locator_beep_period_ms` (see Step 7), exactly like `scene_tracker_geiger.c` — the controller stays display-only so it is reusable by other specs.

- [ ] **Step 7: Implement `scene_locator.c`** — model on `scene_tracker_geiger.c`. `on_enter`: `defender_locator_set_label/set_bounds` from `app->locator_target` (add `struct { char label[28]; int8_t floor, ceil; int8_t (*poll)(void* ctx); void* ctx; } locator_target;` to `DefenderApp`), switch to `DefenderViewLocator`, acquire the speaker (`furi_hal_speaker_acquire(1000)`), start a periodic `FuriTimer` at 1200 ms whose callback reads the latest sample from `app` and, if not stale, `furi_hal_speaker_start(pitch, 0.5f); furi_delay_ms(50); furi_hal_speaker_stop();` then `furi_timer_restart` to `defender_locator_beep_period_ms(...)` (skip restart if 0/muted → keep 1200). `on_event` tick: call `app->locator_target.poll(ctx)` for a fresh RSSI (or mark stale after a 4 s gap), `defender_locator_push(...)`. `on_exit`: stop+free timer, `furi_hal_speaker_stop()/release()` if mine. **For the standalone "Locator Test" entry**, `poll` returns a slow triangle-wave RSSI so the scene is testable without a live radio.

- [ ] **Step 8: Wire.** Append `ADD_SCENE(defender, locator, Locator)` to `defender_scene_config.h`. Add `DefenderViewLocator` to the `DefenderView` enum and `DefenderLocator* locator;` + the `locator_target` struct to `DefenderApp`. In `defender_app.c`: `app->locator = defender_locator_alloc();` and `view_dispatcher_add_view(app->view_dispatcher, DefenderViewLocator, defender_locator_get_view(app->locator));`; free in reverse. In `defender_scene_start.c` `on_event`, add `case DefenderEvtLocatorTest:` → populate `locator_target` (label "Locator Test", bounds -90/-40, triangle poll) and `scene_manager_next_scene(app->scene_manager, DefenderSceneLocator)`.

- [ ] **Step 9: Build** → `build_t4.log`. Expect BUILD OK; "Locator Test" opens the bar and beeps at a cadence that tracks the triangle wave.

- [ ] **Step 10: Commit** `feat(defender): shared RSSI locator (view+scene) generalized from tracker geiger`.

---

## Task 5 — `defender_wifi_tap` (self-contained promiscuous tap + pure frame classifier)

**Files:**
- Create `applications/main/defender/defender_wifi_frame.h` (pure classifier — no esp deps)
- Create `applications/main/defender/defender_wifi_tap.h` / `.c` (firmware: rings + cb + drain)
- Create `tests/test_defender_wifi_frame.c`

**Interfaces produced:**
```c
// defender_wifi_frame.h  (pure — <stdint.h> <stdbool.h> <string.h>)
typedef enum {
    DefWifiOther, DefWifiDeauth, DefWifiDisassoc, DefWifiBeacon, DefWifiProbeResp, DefWifiProbeReq
} DefWifiKind;
typedef struct {
    DefWifiKind kind;
    uint8_t addr1[6], addr2[6], addr3[6]; // dst, src, bssid (mgmt: addr3 = bssid)
    bool     broadcast_addr1;
    uint16_t reason_code;                 // deauth/disassoc only (bytes 24..25), else 0
} DefWifiFrame;
// Parse an 802.11 header. Returns false if len < 24 (no over-read). Only management
// frames (type 0) get a specific kind; everything else → DefWifiOther (still parsed).
static inline bool defender_wifi_classify(const uint8_t* p, uint16_t len, DefWifiFrame* out);

// defender_wifi_tap.h  (firmware)
typedef struct { uint8_t bssid[6]; bool broadcast; uint16_t reason; int8_t rssi; uint32_t ts_ms; }
    DefTapDeauthRec;                              // one deauth/disassoc header
void  defender_wifi_tap_start(uint8_t channel);   // release_bt + start + set MGMT filter + promiscuous cb
void  defender_wifi_tap_stop(void);               // promiscuous off + stop
void  defender_wifi_tap_set_channel(uint8_t ch);
uint32_t defender_wifi_tap_drain_deauth(DefTapDeauthRec* out, uint32_t max); // foreground consume
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_wifi_frame.c` (include `../applications/main/defender/defender_wifi_frame.h`). Build 26-byte headers: bytes 4..9 = addr1, 10..15 = addr2, 16..21 = addr3, 24..25 = reason.
  - deauth: `p[0]=0xC0, p[1]=0x00` → `kind==DefWifiDeauth`; addr1/2/3 extracted; reason from `{0x07,0x00}` == 7.
  - disassoc: `p[0]=0xA0` → `DefWifiDisassoc`.
  - beacon: `p[0]=0x80` → `DefWifiBeacon`, reason==0.
  - probe-resp: `p[0]=0x50` → `DefWifiProbeResp`.
  - probe-req: `p[0]=0x40` → `DefWifiProbeReq`.
  - data frame: `p[0]=0x08` (type 2) → `DefWifiOther`.
  - broadcast: addr1 = `{0xFF×6}` on a deauth → `broadcast_addr1==true`; a unicast addr1 → false.
  - short buffer: `len=20` → returns **false**, out untouched.
  `main()` prints `ALL DEFENDER WIFI FRAME TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_wifi_frame.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_wifi_frame.h`.** `if(len < 24) return false;` `uint16_t fc = p[0] | (p[1]<<8);` `type=(fc & 0x0C)>>2; subtype=(fc & 0xF0)>>4;` copy `addr1=&p[4], addr2=&p[10], addr3=&p[16]`; `broadcast_addr1 = (all six == 0xFF)`. `kind`: if `type==0` map subtype `0x0C→Deauth, 0x0A→Disassoc, 0x08→Beacon, 0x05→ProbeResp, 0x04→ProbeReq, else Other`; else `Other`. `reason_code = (kind==Deauth||Disassoc) && len>=26 ? (p[24]|(p[25]<<8)) : 0`. Return true.

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Implement `defender_wifi_tap.c`** (includes `esp_wifi.h`, `defender_wifi_frame.h`, `defender_wifi_tap.h`, `wlan_hal.h`, `furi.h`). Copy the **ring discipline verbatim** from `wlan_client_scanner.c`: `#define DEF_DEAUTH_RING 48` (spec §3.1), `static volatile uint32_t s_w, s_r;`, `static DefTapDeauthRec s_ring[DEF_DEAUTH_RING];`. Promiscuous cb `def_tap_cb(void* buf, wifi_promiscuous_pkt_type_t type)`: `if(type != WIFI_PKT_MGMT) return;` `const wifi_promiscuous_pkt_t* pkt = buf;` classify `pkt->payload, pkt->rx_ctrl.sig_len`; only for `DefWifiDeauth`/`DefWifiDisassoc` push `{ bssid=addr3, broadcast_addr1, reason_code, rssi=pkt->rx_ctrl.rssi, ts_ms=... }`; **drop-on-full** (`next==s_r → return;`). (`ts_ms`: cb runs in WiFi task — store `pkt->rx_ctrl.timestamp` scaled, or leave 0 and stamp `furi_get_tick()` in the drain; **stamp in the drain** to keep the cb ISR-light and avoid a furi call in the cb.) `start`: `wlan_hal_release_bt(); if(!wlan_hal_is_started()) wlan_hal_start(); if(wlan_hal_is_connected()) wlan_hal_disconnect(); wlan_hal_set_channel(channel);` set the promiscuous **filter to MGMT** via `esp_wifi_set_promiscuous_filter(&(wifi_promiscuous_filter_t){ .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT });` then `wlan_hal_set_promiscuous(true, def_tap_cb);` reset `s_w=s_r=0`. `stop`: `wlan_hal_set_promiscuous(false, NULL); wlan_hal_stop();`. `set_channel`: `wlan_hal_set_channel(ch)`. `drain_deauth`: pop up to `max`, stamp `ts_ms = furi_get_tick()` on each popped record, return count. (No host test on the firmware `.c`; the pure classifier it relies on is covered in Steps 1–4.)

- [ ] **Step 6: Build** → `build_t5.log`. Expect BUILD OK.

- [ ] **Step 7: Commit** `feat(defender): self-contained WiFi promiscuous tap + pure 802.11 classifier`.

---

## Task 6 — `defender_deauth` (deauth/disassoc flood detector + scene + ratemeter)

**Files:**
- Create `applications/main/defender/defender_deauth.h` / `.c` (pure eval + sliding window)
- Create `applications/main/defender/views/defender_ratemeter_view.h` / `.c`
- Create `applications/main/defender/scenes/scene_deauth.c`
- Create `tests/test_defender_deauth.c`
- Modify `defender_scene_config.h`, `defender_app_i.h`, `defender_app.c`, `defender_scene_start.c`

**Interfaces produced:**
```c
// defender_deauth.h  (pure — <stdint.h> <stdbool.h> <string.h>)
#define DEAUTH_RATE_ALERT       20u
#define DEAUTH_WINDOW_MS        1000u
#define DEAUTH_BROADCAST_WEIGHT 2u
typedef struct { uint8_t bssid[6]; bool broadcast; int8_t rssi; uint32_t ts_ms; }
    DefenderDeauthRec;
typedef struct {
    bool     alert;
    uint32_t weighted_count;   // in-window; broadcast counts DEAUTH_BROADCAST_WEIGHT
    uint8_t  top_bssid[6];
    uint16_t top_count;        // raw count for the top offender
    int8_t   top_rssi;
} DefenderDeauthResult;
// Count recs with ts in (now-window, now]; broadcast weighted; alert if weighted >= rate_alert.
void defender_deauth_eval(const DefenderDeauthRec* recs, size_t n, uint32_t now_ms,
                          uint32_t window_ms, uint32_t rate_alert, DefenderDeauthResult* out);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_deauth.c` (include the header + link `defender_deauth.c`). Helper `mk(bssid_last_byte, broadcast, ts)`. `now=100000`, `window=1000`, `rate=20`:
  - `test_below`: 19 unicast recs (bssid A) at ts 99500 → `alert==false`, `weighted_count==19`.
  - `test_at`: 20 unicast → `alert==true`, `weighted_count==20`.
  - `test_broadcast_weight`: 10 broadcast recs → `weighted_count==20`, `alert==true`.
  - `test_broadcast_boundary`: 9 broadcast (18) + 1 unicast (1) = 19 → `alert==false`; add one more broadcast (10 bc = 20) → `alert==true`.
  - `test_window_slide`: 30 recs at ts 98000 (older than now-window) + 5 recs at 99800 → `weighted_count==5`, `alert==false`.
  - `test_top_offender`: 15 recs bssid A + 6 recs bssid B (all in window, unicast) → `top_bssid==A`, `top_count==15`; `weighted_count==21`, `alert==true`.
  `main()` prints `ALL DEFENDER DEAUTH TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_deauth.c applications/main/defender/defender_deauth.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_deauth.c`.** Zero `out`. First pass over `recs`: skip if `(uint32_t)(now_ms - r->ts_ms) >= window_ms` (wrap-safe modular compare, matches `tracker_follow.c`); accumulate `weighted_count += r->broadcast ? DEAUTH_BROADCAST_WEIGHT : 1`. Track a small stack table of `{bssid, count, last_rssi}` (cap 16 distinct; ignore overflow BSSIDs for the top pick) to find `top_count`/`top_bssid`/`top_rssi`. `out->alert = weighted_count >= rate_alert`.

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Ratemeter view** `views/defender_ratemeter_view.{h,c}` — model `{ char title[24]; uint32_t rate; uint8_t top_bssid[6]; uint16_t top_count; int8_t top_rssi; bool alert; }`. Draw: title line; big `"%lu/s"` rate; a top-offender line `"AA:BB:CC:DD:EE:FF x123"`; when `alert`, invert/star the header. `alloc/free` like other simple views.

- [ ] **Step 6: `scene_deauth.c`.** `on_enter`: pick channel (default 6; store `app->deauth_channel`), `defender_wifi_tap_start(app->deauth_channel)`, switch to `DefenderViewRateMeter`, keep a scene-scoped `DefenderDeauthRec window_buf[64]` + count in `DefenderApp`. Channel hop: a `FuriTimer` (500 ms) advancing 1..13 **unless pinned**; pin when an alert fires (stop hopping to keep the count accurate) — implement `app->deauth_pinned`. `on_event` tick: `n = defender_wifi_tap_drain_deauth(tmp, 64);` append into `window_buf` (ring, evicting entries older than `DEAUTH_WINDOW_MS`), `defender_deauth_eval(window_buf, count, furi_get_tick(), DEAUTH_WINDOW_MS, DEAUTH_RATE_ALERT, &res)`, update the view model. On `res.alert`: build a `DefenderAlert{ cat=DefenderCatWifiDeauth, severity=2, offender=res.top_bssid, rssi=res.top_rssi, ts_ms=now, msg=snprintf "Deauth flood ch%u %lu/s" }` and `defender_alert_raise(&app->alert, &a)`, set `deauth_pinned=true`. `InputKeyOk` (via view input→custom event) → populate `locator_target` (label "Rogue AP <bssid>", bounds -90/-40, poll returns the tap's last RSSI for that BSSID) and next_scene Locator. `on_exit`: stop/free hop timer, `defender_wifi_tap_stop()`, reset pin.

- [ ] **Step 7: Wire.** Append `ADD_SCENE(defender, deauth, Deauth)`; add `DefenderViewRateMeter` + the deauth fields (`window_buf`, `deauth_channel`, `deauth_pinned`, hop timer) to `DefenderApp`; alloc/add/free the ratemeter view in `defender_app.c`; in `defender_scene_start.c` `on_event` add `case DefenderEvtDeauth:` → next_scene Deauth.

- [ ] **Step 8: Build** → `build_t6.log`. Expect BUILD OK.

- [ ] **Step 9: Commit** `feat(defender): WiFi deauth/disassoc flood detector (pure eval + ratemeter scene)`.

---

## Task 7 — `defender_ble_tracker` (unwanted-tracker detector, reuses tracker_follow)

**Files:**
- Create `applications/main/defender/defender_ble_tracker_eval.h` (pure decision — includes `tracker_follow.h`)
- Create `applications/main/defender/defender_ble_tracker.h` / `.c` (firmware: TrackerDevice → alert)
- Create `applications/main/defender/scenes/scene_ble_tracker.c`
- Create `tests/test_defender_ble_tracker.c`
- Modify `defender_scene_config.h`, `defender_app_i.h`, `defender_app.c`, `defender_scene_start.c`

**Interfaces produced:**
```c
// defender_ble_tracker_eval.h  (pure — includes "../ble_spam/tracker_follow.h")
// Alert only when a brand-known tracker has escalated to FollowFollowing AND its RSSI is
// rising/steady (within RISE_MARGIN dB of best_rssi → still close / approaching, the
// "rising RSSI while dwelling" stalking signal).
#define BLE_TRACKER_RISE_MARGIN 6
static inline bool defender_ble_tracker_should_alert(
    FollowState st, int8_t rssi, int8_t best_rssi, bool known_brand);

// defender_ble_tracker.h  (firmware — includes ble_tracker_hal.h)
// Scan-driven poll: for each TrackerDevice compute FollowState via tracker_follow_state,
// decide via should_alert; returns index of the strongest fresh offender or -1.
int defender_ble_tracker_poll(const TrackerDevice* devs, uint16_t count, uint32_t now_ms);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_ble_tracker.c` (include the eval header + link `tracker_follow.c`). `best=-50`:
  - `FollowFollowing, rssi=-52, known=true` → **true** (within 6 of best).
  - `FollowFollowing, rssi=-70, known=true` → **false** (dropped 20 dB — moving away).
  - `FollowSuspect, rssi=-50, known=true` → **false** (not yet FOLLOWING).
  - `FollowFollowing, rssi=-50, known=false` → **false** (unknown brand).
  - boundary: `rssi = best - 6 = -56` → **true**; `rssi = best - 7 = -57` → **false**.
  `main()` prints `ALL DEFENDER BLE TRACKER TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_ble_tracker.c applications/main/ble_spam/tracker_follow.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_ble_tracker_eval.h`.** `return known_brand && st == FollowFollowing && (int)(best_rssi - rssi) <= BLE_TRACKER_RISE_MARGIN;`

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Implement `defender_ble_tracker.c`.** `poll`: iterate devices; `FollowState st = tracker_follow_state(d->first_seen_ms, d->last_seen_ms, now_ms);` `bool known = d->kind != TrackerKindUnknown;` if `defender_ble_tracker_should_alert(st, d->rssi, d->best_rssi, known)` and `d->rssi > best_so_far` remember index; return best index or -1.

- [ ] **Step 6: `scene_ble_tracker.c`.** Model on `scene_tracker_scan.c` + `scene_tracker_geiger.c`. `on_enter`: `ble_tracker_hal_start_scan();` render a simple device list (reuse a widget/text list or a minimal custom view). `on_event` tick: `TrackerDevice* d = ble_tracker_hal_get_devices(&count);` `int idx = defender_ble_tracker_poll(d, count, furi_get_tick());` if `idx>=0` raise `DefenderAlert{ cat=DefenderCatBleTracker, severity=2, offender=d[idx].addr, rssi=d[idx].rssi, msg=snprintf "%s following (RSSI up)", ble_tracker_kind_label(d[idx].kind) }` (debounce in the bus handles once-per-MAC-until-stale). `InputKeyOk` on the selected device → `locator_target` (label = kind, bounds -95/-40, poll re-reads that MAC's rssi from `ble_tracker_hal_get_devices`) then next_scene Locator. `on_exit`: `ble_tracker_hal_stop_scan();`.

- [ ] **Step 7: Wire.** Append `ADD_SCENE(defender, ble_tracker, BleTracker)`; add any needed view id; `case DefenderEvtBleTracker:` → next_scene in start.

- [ ] **Step 8: Build** → `build_t7.log`. Expect BUILD OK.

- [ ] **Step 9: Commit** `feat(defender): BLE unwanted-tracker detector reusing tracker_follow + locator handoff`.

---

## Task 8 — `defender_ble_spam` (BLE-spam flood detector)

**Files:**
- Create `applications/main/defender/defender_ble_spam.h` / `.c` (pure eval)
- Create `applications/main/defender/scenes/scene_ble_spam.c`
- Create `tests/test_defender_ble_spam.c`
- Modify `defender_scene_config.h`, `defender_app_i.h`, `defender_app.c`, `defender_scene_start.c`

**Interfaces produced:**
```c
// defender_ble_spam.h  (pure)
#define BLE_SPAM_ALERT        25u   // spam-payload adverts / window
#define BLE_MAC_FLOOD_ALERT   40u   // distinct locally-administered MACs / window
#define BLE_SPAM_WINDOW_MS    1000u
#define BLE_CID_APPLE     0x004Cu    // Continuity
#define BLE_CID_MICROSOFT 0x0006u    // SwiftPair
#define BLE_UUID_FASTPAIR 0xFE2Cu    // Google Fast Pair
typedef struct {
    uint8_t  addr[6];
    bool     is_random_mac;   // locally-administered bit set (addr[0] & 0x02)
    uint16_t company_id;      // manufacturer-data company id, 0 if none
    uint16_t service_uuid;    // 0 if none
    uint32_t ts_ms;
} DefenderBleSpamRec;
typedef struct {
    bool     alert;
    uint16_t continuity;      // in-window
    uint16_t fastpair;
    uint16_t swiftpair;
    uint16_t distinct_random_macs;
} DefenderBleSpamResult;
void defender_blespam_eval(const DefenderBleSpamRec* recs, size_t n, uint32_t now_ms,
    uint32_t window_ms, uint32_t spam_alert, uint32_t mac_flood_alert, DefenderBleSpamResult* out);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_ble_spam.c` (header + link `.c`). `now=50000, window=1000, spam=25, macflood=40`:
  - `test_continuity_below`: 24 recs `company_id=0x004C` (fixed MAC) → `continuity==24`, `alert==false`.
  - `test_continuity_at`: 25 → `alert==true`.
  - `test_mixed_payload`: 10 Continuity + 10 FastPair(`service_uuid=0xFE2C`) + 5 SwiftPair(`company_id=0x0006`) = 25 spam → `alert==true`, fields 10/10/5.
  - `test_mac_flood`: 40 recs each a **distinct** random MAC (`is_random_mac=true`, `addr[0]|=0x02`), benign payload → `distinct_random_macs==40`, `alert==true` (via MAC flood).
  - `test_mac_not_double_counted`: 40 recs all the **same** random MAC → `distinct_random_macs==1`, `alert==false`.
  - `test_window_slide`: 30 Continuity at ts 48000 (stale) + 5 at 49500 → `continuity==5`, `alert==false`.
  `main()` prints `ALL DEFENDER BLE SPAM TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_ble_spam.c applications/main/defender/defender_ble_spam.c && /tmp/dt"`

- [ ] **Step 3: Implement `defender_ble_spam.c`.** In-window filter (wrap-safe). Count `continuity += company_id==BLE_CID_APPLE`; `fastpair += service_uuid==BLE_UUID_FASTPAIR`; `swiftpair += company_id==BLE_CID_MICROSOFT`. `distinct_random_macs`: maintain a small stack set (cap 64) of seen random MACs (`is_random_mac` only); count distinct. `spam_total = continuity+fastpair+swiftpair`; `out->alert = spam_total >= spam_alert || distinct_random_macs >= mac_flood_alert`.

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: `scene_ble_spam.c`.** `on_enter`: `ble_tracker_hal_set_sweep(true); ble_tracker_hal_start_scan();` (sweep captures all adverts). Feed the eval from a scene-scoped window of `DefenderBleSpamRec`. **Note:** `ble_tracker_hal` currently surfaces `TrackerDevice` (classified), not raw mfg/UUID per advert. For Phase 1, derive `company_id` from `TrackerKind` where possible (AirTag/PP/Nearby → 0x004C; SmartTag → 0x0075) and `is_random_mac` from `addr[0] & 0x02`, using each device's `last_seen_ms` as the advert timestamp. Reuse the ratemeter view (title "BLE Spam") to show `continuity/fastpair/swiftpair` counts + MAC-flood rate. On `alert`: raise `DefenderAlert{ cat=DefenderCatBleSpam, severity=2, offender=<first offending addr>, msg = "BLE spam: Continuity %u/s" / "FastPair flood" / "SwiftPair flood" }`. `on_exit`: `ble_tracker_hal_stop_scan(); ble_tracker_hal_set_sweep(false);`. **Flag for controller:** exposing raw manufacturer-data/UUID per advert from `ble_tracker_hal` is a follow-up (ambiguity #3) — Phase 1 approximates from `TrackerKind`.

- [ ] **Step 6: Wire.** Append `ADD_SCENE(defender, ble_spam, BleSpam)`; `case DefenderEvtBleSpam:` → next_scene.

- [ ] **Step 7: Build** → `build_t8.log`. Expect BUILD OK.

- [ ] **Step 8: Commit** `feat(defender): BLE-spam flood detector (Continuity/FastPair/SwiftPair + MAC flood)`.

---

## Task 9 — `defender_jammer` (Sub-GHz jam / RF-energy monitor + waterfall)

**Files:**
- Create `applications/main/defender/defender_jammer.h` / `.c` (pure floor-EMA state machine)
- Create `applications/main/defender/views/defender_waterfall_view.h` / `.c`
- Create `applications/main/defender/scenes/scene_jammer.c`
- Create `tests/test_defender_jammer.c`
- Modify `defender_scene_config.h`, `defender_app_i.h`, `defender_app.c`, `defender_scene_start.c`

**Interfaces produced:**
```c
// defender_jammer.h  (pure — <stdint.h> <stdbool.h> <math.h>)
#define JAM_DB_OVER      18.0f   // dB above the noise floor
#define JAM_SUSTAIN_MS   400u    // must hold this long, with no decoded packet
typedef struct {
    float    floor_ema;    bool have_floor;
    uint32_t over_since_ms;         // 0 = not currently over-threshold
    bool     jamming;
} DefenderJammerState;
// Feed one RSSI sample (dBm) + whether a valid packet decoded since the last feed.
// Floor tracks minima (fast down, slow up). Jam = sustained (>= JAM_SUSTAIN_MS) energy
// >= floor+JAM_DB_OVER with zero decoded packets in that span.
void defender_jammer_feed(DefenderJammerState* st, float rssi, bool packet_decoded, uint32_t now_ms);
```

- [ ] **Step 1: Write failing test.** `tests/test_defender_jammer.c` (header + link `.c`). `DefenderJammerState st = {0};`
  - `test_floor_tracks`: feed `-95.0f, false, t` for t=0,100,...,1000 → `st.floor_ema` within 1.0 of -95, `st.jamming==false`.
  - `test_jam`: prime floor to ≈-95 (feed -95 a few times), then feed `-60.0f (35 dB over), packet_decoded=false` at t=2000, 2100, 2200, 2300 → `jamming==false` (not yet 400 ms); at t=2400 → `jamming==true`.
  - `test_spike_with_packet`: primed floor -95; feed `-60, packet_decoded=true` at 3000..3500 → `jamming==false` (a real transmitter decodes) and `over_since_ms==0`.
  - `test_brief_spike_no_packet`: primed; feed `-60,false` at 4000, then `-90,false` at 4200 (energy gone before 400 ms) → `jamming==false`, `over_since_ms==0`.
  `main()` prints `ALL DEFENDER JAMMER TESTS PASSED`.

- [ ] **Step 2: Run → FAILS.**
  `wsl.exe -- bash -lc "... gcc -Wall -Wextra -o /tmp/dt tests/test_defender_jammer.c applications/main/defender/defender_jammer.c -lm && /tmp/dt"`

- [ ] **Step 3: Implement `defender_jammer.c`.** First sample: `floor_ema=rssi; have_floor=true`. Else asymmetric EMA: `if(rssi < floor_ema) floor_ema = floor_ema*0.5f + rssi*0.5f;` (chase minima fast) `else floor_ema = floor_ema*0.99f + rssi*0.01f;` (drift up slowly). `float thresh = floor_ema + JAM_DB_OVER;` `if(rssi >= thresh && !packet_decoded) { if(over_since_ms==0) over_since_ms=now_ms; if((uint32_t)(now_ms - over_since_ms) >= JAM_SUSTAIN_MS) jamming=true; } else { over_since_ms=0; jamming=false; }`.

- [ ] **Step 4: Run → PASSES.**

- [ ] **Step 5: Waterfall view** `views/defender_waterfall_view.{h,c}` (§3.10 — shared, will also serve nRF24 in Phase 2). Model `{ uint8_t col[MAXBINS]; uint8_t nbins; int8_t peak_bin; char label[24]; uint8_t hist[HISTW][MAXBINS]; uint8_t hist_head; }` with `#define MAXBINS 64`, `#define HISTW 64`. Provide `defender_waterfall_view_push(View*, const uint8_t* col, uint8_t nbins, int8_t peak_bin)` — appends a column, scrolls. Draw: render the scrolling history as columns (coarse mono on the 128×64 canvas; brightness by intensity) + a bottom dB bar. No radio knowledge.

- [ ] **Step 6: `scene_jammer.c`.** `on_enter`: `SubGhzThresholdRssi* rssi = subghz_threshold_rssi_alloc();` park on 433.92 MHz via `subghz_txrx` tuning (mirror `subghz_frequency_analyzer_worker.c`'s RSSI poll loop), start a `FuriTimer` (~50 ms) that reads RSSI (`subghz_threshold_get_rssi_data(rssi, live_rssi).rssi`), pushes a single-bin column into the waterfall, and calls `defender_jammer_feed(&app->jammer, live, packet_decoded, furi_get_tick())`. **`packet_decoded`**: read from the Sub-GHz decoder's packet-complete signal if exposed; if not available in Phase 1, pass `false` and note the RSSI-only fallback (ambiguity #2 / spec open item #8). On `st.jamming` transitioning true: raise `DefenderAlert{ cat=DefenderCatSubghzJam, severity=2, offender={0}, rssi=(int8_t)live, msg=snprintf "RF jam 433.92 +%ddB", (int)(live - st.floor_ema) }`. `InputKeyOk` → `locator_target` (label "Jammer 433.92", bounds -110/-40, poll returns the live CC1101 RSSI) → next_scene Locator. `on_exit`: stop/free timer, `subghz_threshold_rssi_free(rssi)`, release the CC1101 tuning. **Guard** the whole scene behind a runtime probe (Cardputer-ADV has SubGHz; if the CC1101 probe fails, show a "No Sub-GHz" popup and back out) — do not board-gate the app.

- [ ] **Step 7: Wire.** Append `ADD_SCENE(defender, jammer, Jammer)`; add `DefenderViewWaterfall` + `DefenderJammerState jammer;` to `DefenderApp`; alloc/add/free the waterfall view; `case DefenderEvtJammer:` → next_scene.

- [ ] **Step 8: Build** → `build_t9.log`. Expect BUILD OK.

- [ ] **Step 9: Commit** `feat(defender): Sub-GHz jam detector (floor-EMA eval + waterfall + locator)`.

---

## Phase 2 — task outline (titles only; do NOT detail here)

- P2-T1: `defender_eviltwin` (+ SD `ap_baseline.csv`, learn-network) — §3.2
- P2-T2: `defender_beaconflood` (unique-BSSID/sec fixed hash set) — §3.3
- P2-T3: `defender_karma` (BSSID→distinct-SSID fanout) — §3.4
- P2-T4: `defender_fingerprint` (`defender_fp_classify` + `attacker_oui.csv`) — §3.5
- P2-T5: `defender_nrf_spectrum` (126-channel `nrf24_hw_listen_rpd` occupancy + waterfall) — §3.9
- P2-T6: `odid_parser` (ported opendroneid parse path, pure) — §3.11
- P2-T7: `defender_remoteid` (WiFi NaN + BLE taps → drone list/detail + locator) — §3.11
- P2-T8: `defender_integrated` (single shared WiFi tap fan-out + BLE, categorized alert feed) — §3.12
- P2-T9: `defender_logger` PCAP implementation (un-stub `pcap_begin/write/end`, rotation) — §6.2
- P2-T10: `defender_purple` fire-and-verify harnesses (deauth / BLE-spam / Sub-GHz jam) — §3.13
- P2-T11: WiFi+BLE coexistence prototype / time-slicing decision for integrated mode — open item #1

---

## Self-Review

**Spec coverage (Phase 1 scope):**
- Shared components owned here: `defender_alert` (§4) → T2; `defender_logger` (§6.2, pcap stubbed) → T3; `defender_locator` (§6.1) → T4. ✅
- Self-contained WiFi tap on existing `wlan_hal_*`, headers-only rings, drop-on-full (§2, §7) → T5. ✅
- Four highest-value detectors: deauth §3.1 → T6; BLE tracker §3.6 → T7; BLE spam §3.7 → T8; Sub-GHz jam §3.8 → T9. ✅
- Every `*_eval`/classifier is pure + host-tested per §9 (deauth rate/broadcast/window/top-offender; blespam Continuity/FastPair/SwiftPair + MAC flood; jammer floor-EMA + sustained-vs-spike; wifi classifier; debounce; CSV formatting; locator mapping; tracker decision). ✅
- Foundation (T1–T5) precedes detectors (T6–T9); registration order 68, MENUEXTERNAL, stack 8 KB (§5). ✅
- Deferred exactly as instructed: eviltwin, beaconflood, karma, fingerprint, nrf_spectrum, remoteid+odid_parser, integrated, purple, real PCAP → Phase 2 outline. ✅

**API verification (real, not invented):**
- WiFi RX: `wlan_hal_set_promiscuous(bool, wifi_promiscuous_cb_t)`, `wlan_hal_set_channel`, `wlan_hal_start/stop/is_started/is_connected/disconnect`, `wlan_hal_release_bt` — all confirmed in `wlan_hal.h`. Ring/cb/`rx_ctrl.rssi`/`rx_ctrl.sig_len`/FC parse confirmed in `wlan_client_scanner.c`.
- BLE: `ble_tracker_hal_start_scan/stop_scan/get_devices/set_sweep`, `TrackerKind`, `TrackerDevice{addr,kind,rssi,best_rssi,first_seen_ms,last_seen_ms}`, `ble_tracker_kind_label`, `tracker_follow_state`, `FollowState`, `FOLLOW_*_MS` — confirmed.
- Sub-GHz: `subghz_threshold_rssi_alloc/free`, `subghz_threshold_get_rssi_data() → {float rssi; bool is_above;}` — confirmed.
- SD: `wlan_pcap_open/write_packet/close`, `wlan_oui_load/lookup/free`, `storage_*` — confirmed (pcap wrapped later).
- Feedback: `furi_hal_light_blink_start(LightRed,0xFF,200,400)`, `furi_hal_light_set_rgb_all`, `furi_hal_speaker_acquire/is_mine/start/stop/release` — confirmed from `scene_tracker_scan.c` / `scene_tracker_geiger.c`.
- **nRF24 (Phase 2, verified now per instruction):** spec assumed `nrf24_hw_listen_rpd` — **CONFIRMED real**, exact signature `uint8_t nrf24_hw_listen_rpd(uint8_t channel)` in `nrf24/nrf24_hw.h`, must be wrapped in `nrf24_hw_acquire()/nrf24_hw_release()`. No rename needed.
- Registration: `fam_config.py` `APPS` (manual list, no auto-glob) + `application.fam App(...)`; app sources auto-globbed (no `sources=` in wlan/ble_spam manifests). Confirmed.
- Host-test harness: plain C in `tests/`, `assert`-based, compiled via `wsl.exe … gcc … <test.c> [pure .c] && run`. Confirmed against `test_tracker_follow.c` / `test_pmkid_frames.c` and the tracker-bugsweep plan's exact command.

**Placeholder scan:** every pure module ships real code (formulas, constants, struct layouts) and every host test lists concrete inputs→expected values. No "similar to Task N", no "add error handling", no TODO in Phase-1 steps. Firmware scenes give concrete API call sequences and the sibling file to model on.

**Type consistency:** `DefenderAlert`/`DefenderCategory` defined once in `defender_alert.h` and consumed by logger, all detectors, and scenes. Pure evals never include `furi.h`/`esp_*`/`gui/*` (host-compilable); firmware wrappers include the pure header + platform headers. `DefTapDeauthRec` (tap output) and `DefenderDeauthRec` (eval input) are distinct but field-compatible — the drain copies bssid/broadcast/reason/rssi/ts across; noted so an implementer doesn't assume they're the same struct.

**Resolved ambiguities:**
1. **WiFi↔BLE mutual exclusion (spec open item #1):** `wlan_hal_release_bt()` needs a reboot to restore BLE, so Phase 1 does **not** switch WiFi and BLE scenes freely in one session — each detector scene self-contains one radio; integrated WiFi+BLE is Phase 2. *Controller decision still needed:* whether the start submenu should warn the user that opening a WiFi detector disables BLE until reboot.
2. **Sub-GHz "no valid packet" signal (open item #8):** if the decoder exposes no packet-complete hook, T9 passes `packet_decoded=false` (RSSI-only jam heuristic). *Needs controller confirmation* on whether the RSSI-only fallback is acceptable for v1 or the decoder hook must be exposed first.
3. **`ble_tracker_hal` raw advert fields:** T8 needs per-advert manufacturer company-id / service-UUID, which `ble_tracker_hal` does not surface today (it returns classified `TrackerDevice`). Phase 1 approximates `company_id`/`is_random_mac` from `TrackerKind` + `addr`. *Follow-up:* extend `ble_tracker_hal` to expose raw mfg/UUID (or a spam-classification hook) for a precise §3.7.
4. **Icon:** spec's `application.fam` used `fap_icon="icon.png"`, but this port has no Pillow to compile PNGs (per prior build notes), so this plan uses the built-in `icon="A_Plugins_14"` (as `wlan`/`ble_spam` do) and ships no `icon.png`. Deviation is intentional.
