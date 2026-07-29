# Loot Session-Grouping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Group pure-evidence WiFi captures into a per-boot session folder `/ext/loot/<YYYYMMDD_HHMMSS>/` with a `manifest.csv` index, so a red-team operator can grab one folder per engagement and see what/when/for-which-target was captured. State-stores (saved passwords, LAN cache) are untouched; handshakes keep their configurable save-path but get a manifest entry.

**Architecture:** A new `wlan_loot` helper owns the per-boot session directory (fixed at first WiFi-app launch via persistent file-statics; the folder is created lazily on first capture) and a mutex-guarded manifest writer. The three fixed-path evidence writers (PMKID, Evil Portal creds, Live creds) redirect into the session folder and log to the manifest; the handshake completion path logs to the manifest only.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Firmware build only.

## Global Constraints

- Build via `build_ci.bat` from Git Bash, SYNCHRONOUS (never backgrounded), timeout 600000 ms:
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = tail `Project build complete` + `=== BUILD OK ===`, no `FAILED ===`. `build_cardputer_adv/` validly configured → incremental; do NOT delete it. `build_*.log` gitignored.
- Session semantics: ONE folder per boot. `wlan_loot_init()` fixes the session id on the first WiFi-app launch; the file-static state persists across app relaunches within a boot (so all captures this boot share one folder), and resets only on reboot. The folder is created lazily on the first capture (no empty folders for capture-less runs).
- State-stores are OUT of scope and unchanged: `wlan_passwords.c` (`/ext/wifi/<ssid>.txt`), `wlan_lan_cache.c` (`/ext/wifi/lan/…`). Handshake PCAPs keep their configurable `hs_settings.save_path`.
- All redirected evidence files are write-only loot (never read back by the app) — confirmed safe to move.
- Manifest row format: `timestamp,type,target,detail` with `timestamp` = `YYYY-MM-DD HH:MM:SS`. `target`/`detail` commas/newlines are neutralized to spaces.
- Subagents on opus/haiku (Sonnet-5 gate). Repo git on `master`; commit per task.

---

### Task 1: `wlan_loot` module (session dir + manifest) + init hook

**Files:**
- Create: `applications/main/wlan_app/wlan_loot.h`
- Create: `applications/main/wlan_app/wlan_loot.c`
- Modify: `applications/main/wlan_app/wlan_app.c` (call `wlan_loot_init()` once at app startup)

**Interfaces:**
- Produces (used by Tasks 2-4):
  - `void wlan_loot_init(void);`
  - `const char* wlan_loot_session_dir(void);`
  - `void wlan_loot_record(const char* type, const char* target, const char* detail);`

- [ ] **Step 1: Header**

Create `applications/main/wlan_app/wlan_loot.h`:
```c
#pragma once

/* Per-boot loot session grouping. Pure-evidence captures (PMKID, portal creds,
 * live creds) write into /ext/loot/<YYYYMMDD_HHMMSS>/ and are logged to
 * <session>/manifest.csv. State-stores (saved passwords, LAN cache) are NOT
 * affected. The session id is fixed at the first wlan_loot_init() of the boot and
 * the folder is created lazily on the first capture. */

/* Fix the session id + alloc the internal lock. Call once at WiFi-app startup
 * (single-threaded). No-op after the first call of the boot (so the session
 * persists across app relaunches until reboot). */
void wlan_loot_init(void);

/* "/ext/loot/<YYYYMMDD_HHMMSS>" (no trailing slash), creating /ext/loot and the
 * session folder on first call. Never NULL. */
const char* wlan_loot_session_dir(void);

/* Append one row to <session>/manifest.csv: timestamp,type,target,detail (creates
 * the manifest + header on first write). target/detail may be "" (not NULL);
 * their separators are neutralized. */
void wlan_loot_record(const char* type, const char* target, const char* detail);
```

- [ ] **Step 2: Implementation**

Create `applications/main/wlan_app/wlan_loot.c`:
```c
#include "wlan_loot.h"
#include <storage/storage.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <stdio.h>
#include <string.h>

#define LOOT_ROOT "/ext/loot"

static char s_session_dir[48]; /* "/ext/loot/YYYYMMDD_HHMMSS" */
static bool s_dir_created = false;
static FuriMutex* s_mutex = NULL;

void wlan_loot_init(void) {
    if(s_mutex) return; /* already fixed for this boot */
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        s_session_dir, sizeof(s_session_dir),
        LOOT_ROOT "/%04u%02u%02u_%02u%02u%02u",
        (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
        (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    s_dir_created = false;
    s_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
}

const char* wlan_loot_session_dir(void) {
    if(!s_mutex) wlan_loot_init();
    furi_mutex_acquire(s_mutex, FuriWaitForever);
    if(!s_dir_created) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(storage, LOOT_ROOT);
        storage_common_mkdir(storage, s_session_dir);
        furi_record_close(RECORD_STORAGE);
        s_dir_created = true;
    }
    furi_mutex_release(s_mutex);
    return s_session_dir;
}

/* Copy src into dst (bounded), replacing , \r \n with spaces so a field can't
 * break the CSV row. */
static void loot_csv_field(char* dst, size_t dst_sz, const char* src) {
    size_t j = 0;
    if(src) {
        for(size_t i = 0; src[i] && j < dst_sz - 1; i++) {
            char c = src[i];
            dst[j++] = (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    dst[j] = '\0';
}

void wlan_loot_record(const char* type, const char* target, const char* detail) {
    const char* dir = wlan_loot_session_dir(); /* acquires+releases the lock */
    furi_mutex_acquire(s_mutex, FuriWaitForever);
    Storage* storage = furi_record_open(RECORD_STORAGE);

    char path[80];
    snprintf(path, sizeof(path), "%s/manifest.csv", dir);
    bool exists = storage_common_stat(storage, path, NULL) == FSE_OK;

    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(!exists) {
            const char* hdr = "timestamp,type,target,detail\n";
            storage_file_write(f, hdr, strlen(hdr));
        }
        DateTime dt;
        furi_hal_rtc_get_datetime(&dt);
        char t[40], g[64], d[80];
        loot_csv_field(t, sizeof(t), type);
        loot_csv_field(g, sizeof(g), target);
        loot_csv_field(d, sizeof(d), detail);
        char line[220];
        int n = snprintf(
            line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%s\n",
            (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
            (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second, t, g, d);
        if(n > 0) {
            if(n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            storage_file_write(f, line, (size_t)n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    furi_mutex_release(s_mutex);
}
```
(No deadlock: `wlan_loot_session_dir()` releases the lock before returning, then `wlan_loot_record` re-acquires it — sequential, not nested.)

- [ ] **Step 3: Init hook**

In `applications/main/wlan_app/wlan_app.c`, add `#include "wlan_loot.h"` near the top includes, and call `wlan_loot_init();` once early in the app-alloc/entry path (near where the app struct + records are set up — a single-threaded point before any capture runs).

- [ ] **Step 4: Build** — run the standard build (`build_loot1.log`). Expected `=== BUILD OK ===`; the new `wlan_loot.c` auto-globs into the build.

- [ ] **Step 5: Commit**
```bash
git add applications/main/wlan_app/wlan_loot.h applications/main/wlan_app/wlan_loot.c applications/main/wlan_app/wlan_app.c
git commit -m "feat(wlan): loot session directory + manifest helper"
```

---

### Task 2: Redirect PMKID into the session folder + manifest

**Files:**
- Modify: `applications/main/wlan_app/wlan_pmkid_out.c`

**Interfaces:** consumes Task 1. `wlan_pmkid_save` now writes `<session>/pmkid.22000` and records each capture.

- [ ] **Step 1: Redirect the path + record**

In `applications/main/wlan_app/wlan_pmkid_out.c`, add `#include "wlan_loot.h"` near the includes. In `wlan_pmkid_save`, replace the fixed path + mkdir with the session path. The current body opens `"/ext/wifi/pmkid.22000"` (after `storage_common_mkdir(storage, "/ext/wifi")`). Change to:
```c
    const char* dir = wlan_loot_session_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/pmkid.22000", dir);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
```
(Remove the now-unneeded `storage_common_mkdir(storage, "/ext/wifi")` — `wlan_loot_session_dir()` creates the loot dir. Keep the rest of the write loop identical.)

After the file is written and closed (right before `storage_file_free(f)`), record it:
```c
        storage_file_close(f);
        wlan_loot_record("pmkid", ssid, "pmkid.22000");
    }
    storage_file_free(f);
```
(Place the `wlan_loot_record` INSIDE the `if(storage_file_open(...))` block so it only logs on a successful write. `ssid` is the existing parameter.)

- [ ] **Step 2: Build** — `build_loot2.log`. Expected `=== BUILD OK ===`.

- [ ] **Step 3: Commit**
```bash
git add applications/main/wlan_app/wlan_pmkid_out.c
git commit -m "feat(wlan): PMKID captures write to the loot session folder + manifest"
```

---

### Task 3: Redirect Evil Portal creds + Live creds + manifest

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_evil_portal.c` (`ep_open_cred_file`)
- Modify: `applications/main/wlan_app/scenes/scene_live_creds.c` (`lc_open_csv`)

**Interfaces:** consumes Task 1.

- [ ] **Step 1: Evil Portal creds → session folder**

In `scene_evil_portal.c`, add `#include "../wlan_loot.h"` near the includes. In `ep_open_cred_file`, replace the mkdir + path build:
```c
    s_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_storage, "/ext/wifi");
    storage_common_mkdir(s_storage, "/ext/wifi/evil_portal");

    char safe[33];
    ep_sanitize_filename(
        app->evil_portal_ssid[0] ? app->evil_portal_ssid : "portal", safe, sizeof(safe));

    char path[160];
    snprintf(path, sizeof(path), "/ext/wifi/evil_portal/%s_creds.csv", safe);
    bool exists = storage_common_stat(s_storage, path, NULL) == FSE_OK;
```
with:
```c
    s_storage = furi_record_open(RECORD_STORAGE);

    char safe[33];
    ep_sanitize_filename(
        app->evil_portal_ssid[0] ? app->evil_portal_ssid : "portal", safe, sizeof(safe));

    char fname[64];
    snprintf(fname, sizeof(fname), "evil_portal_%s_creds.csv", safe);
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", wlan_loot_session_dir(), fname);
    bool exists = storage_common_stat(s_storage, path, NULL) == FSE_OK;
```
Then, right after the header is written for a new file (`if(!exists) { ...write hdr... }`), add the manifest record:
```c
    if(!exists) {
        const char* hdr = "timestamp,user,password\n";
        storage_file_write(s_cred_file, hdr, strlen(hdr));
        wlan_loot_record("portal_creds", app->evil_portal_ssid, fname);
    }
```

- [ ] **Step 2: Live creds → session folder**

In `scene_live_creds.c`, add `#include "../wlan_loot.h"`. In `lc_open_csv`, replace:
```c
    s_lc_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_lc_storage, "/ext/wifi");
    storage_common_mkdir(s_lc_storage, "/ext/wifi/live_creds");
    const char* path = "/ext/wifi/live_creds/creds.csv";
    bool exists = storage_common_stat(s_lc_storage, path, NULL) == FSE_OK;
```
with:
```c
    s_lc_storage = furi_record_open(RECORD_STORAGE);
    char path[128];
    snprintf(path, sizeof(path), "%s/live_creds.csv", wlan_loot_session_dir());
    bool exists = storage_common_stat(s_lc_storage, path, NULL) == FSE_OK;
```
(`path` was a `const char*` before — it's now a `char[128]`; the later `storage_file_open(s_lc_file, path, ...)` call is unchanged and works with the array.) After the header write for a new file, add:
```c
    if(!exists) {
        const char* hdr = "timestamp,victim_ip,proto,host,peer,user,secret,raw\n";
        storage_file_write(s_lc_file, hdr, strlen(hdr));
        wlan_loot_record("live_creds", "", "live_creds.csv");
    }
```

- [ ] **Step 3: Build** — `build_loot3.log`. Expected `=== BUILD OK ===`, no unused-variable warnings (the removed `/ext/wifi` mkdirs and the `const char*`→array change must leave no dangling references).

- [ ] **Step 4: Commit**
```bash
git add applications/main/wlan_app/scenes/scene_evil_portal.c applications/main/wlan_app/scenes/scene_live_creds.c
git commit -m "feat(wlan): Evil Portal + Live creds write to the loot session folder + manifest"
```

---

### Task 4: Manifest-record completed handshakes

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_handshake.c`

**Interfaces:** consumes Task 1. Handshake PCAPs stay at their configurable `save_path`; only a manifest row is added so the index is complete.

- [ ] **Step 1: Include + single-target completion record**

In `scene_handshake.c`, add `#include "../wlan_loot.h"` near the includes. In the tick handler where a single-target handshake first completes — the block `if(hs_single_complete() && !app->handshake_complete) { app->handshake_complete = true; ... }` — add, inside that block:
```c
            wlan_loot_record("handshake", s_single.ssid, "");
```

- [ ] **Step 2: Channel-mode completion record**

In `hsc_process_packet`, where a target first becomes complete — the block `if(!was_complete && now_complete) { ... memorize s_latest_hs_ssid ... }` — add at the end of that block:
```c
        wlan_loot_record("handshake", t->ssid, "");
```
(`t` is the `HsChannelTarget*` in scope; this runs in the scene tick/drain context, not the RX callback, so storage ops are safe.)

- [ ] **Step 3: Build** — `build_loot4.log`. Expected `=== BUILD OK ===` and a regenerated `Flipper-cardputer_adv-merged.bin`.

- [ ] **Step 4: Commit**
```bash
git add applications/main/wlan_app/scenes/scene_handshake.c
git commit -m "feat(wlan): log completed handshakes to the loot manifest"
```

---

### Task 5: On-hardware validation

**Files:** none (manual; needs the Cardputer ADV on COM12).

- [ ] **Step 1: Flash** — `python -m esptool --chip esp32s3 --port COM12 -b 460800 write_flash 0x0 Flipper-cardputer_adv-merged.bin`
- [ ] **Step 2: Session folder** — power on, run a PMKID capture + an Evil Portal session (capture a cred) + a handshake. Confirm a single `/ext/loot/<YYYYMMDD_HHMMSS>/` folder exists containing `pmkid.22000`, `evil_portal_<ssid>_creds.csv`, `live_creds.csv` (if MiTM used), and `manifest.csv`; confirm handshakes are still in the configured handshakes folder.
- [ ] **Step 3: Manifest** — open `manifest.csv`; confirm one row per capture with `timestamp,type,target,detail` (pmkid/portal_creds/live_creds/handshake rows, correct SSIDs, sane times).
- [ ] **Step 4: Per-boot grouping** — without rebooting, exit and re-enter the WiFi app, capture again; confirm captures land in the SAME session folder. Reboot, capture again; confirm a NEW session folder. Confirm `/ext/wifi/<ssid>.txt` password store + LAN cache still work (unchanged).
- [ ] **Step 5: Record** — `git commit --allow-empty -m "test: loot session-grouping validated on Cardputer ADV hardware"`

---

## Self-Review Notes

- **Roadmap P1 #6 (session folder + timestamps + index):** Tasks 1-4 — per-boot session folder, manifest index with timestamps, evidence redirected, handshakes indexed. ✓
- **Scope kept:** state-stores (passwords, LAN cache) and the handshake save-path setting untouched; only write-only evidence redirected. ✓
- **Concurrency:** manifest + lazy mkdir guarded by a mutex; per-boot session via persistent file-statics + `if(s_mutex) return` in init. ✓
- **CSV safety:** manifest fields neutralize `,`/`\r`/`\n`. ✓
- **Type consistency:** `wlan_loot_session_dir`/`wlan_loot_record`/`wlan_loot_init` defined in Task 1, consumed in Tasks 2-4; `scene_live_creds` `path` changes from `const char*` to `char[128]` within Task 3 (self-contained).
- **Deferred:** BLE AutoWalk (`/ext/ble/auto_walk.csv`, a different app) not included; could join the manifest later.
