# NFC Share — Consolidate to One App + Confirm-Saving Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the standalone **NFC Share** app the single NFC file-transfer entry point by fully removing the superseded Bus Tools NFC proof-of-concept scenes, and show the saved path on a successful receive so file-saving is visibly confirmed.

**Architecture:** Delete the Bus Tools ST25R NFC scene + view and every reference to them (the standalone `nfc_share` app already carries the proven send/receive engine). Add one `dest_name` field to `NfcShareStatus` that the receive path fills with the actual (dedup-resolved) saved filename, and render a "Saved to SD" confirmation line in the progress view. Mirror all edits into the cardputer tree.

**Tech Stack:** C (ESP32-S3, esp-idf v5.4.1), this repo's Flipper-port furi APIs (`gui/scene_manager`, `gui/view_dispatcher`, `gui/modules/submenu`, `gui/elements`), the in-tree `nfc_share` engine. No new dependencies. No protocol change.

## Global Constraints

- **Two trees, kept in sync:** every source edit lands in `NEW_v2/src_m5stick_s3` first, then is mirrored byte-identically into `NEW_v2/src_cardputer_adv` (Task 3). `nfc_share/` and `bus_tools/` are currently byte-identical between the trees.
- **Build ONLY via the PowerShell tool** (not cmd-through-bash). Invoke the bat by FULL PATH; the spawned cmd does not inherit cwd.
  - m5stick: `& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"` → watch `build_m5stick_s3.log` end with `SENTINEL_OK`, no errors.
  - cardputer: `cmd /c '"C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_cardputer_adv\build_adv.bat" < NUL > build_adv.log 2>&1'` → `build_adv.log` contains `=== SUCCESS ===` and produces `Flipper-cardputer_adv-merged.bin`. (The notifier's "exit 0" is from the trailing `pause`; always grep the log for `=== SUCCESS ===` / `=== BUILD FAILED`.)
  - `IDF_TOOLS_PATH=C:\Users\stone\.espressif` (already set inside both bats).
- **Flash (m5stick):** COM10. `python -m esptool --chip esp32s3 --port COM10 --baud 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 Flipper-m5stick_s3-merged.bin`.
- **git:** m5stick tree is a git repo on branch `main` (commit directly, matching the existing nfc_share history). The cardputer tree is **NOT** under git — nothing to commit there; parity is maintained by copying files.
- **No behavior change to:** the nsh wire protocol, the engine send/receive control flow, the file browser, or Bus Tools' other scenes (I2C Scanner, UART Console/Monitor, SPI Flash Reader stub).

---

### Task 1: Remove the Bus Tools ST25R NFC scene + view (full removal)

Delete the Phase 1–3 proof-of-concept NFC path from Bus Tools so it reverts to an I2C/UART utility and the only NFC file-share is the standalone app. No dead code left behind.

**Files:**
- Delete: `applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c`
- Delete: `applications/main/bus_tools/bus_tools_nfc_view.c`
- Delete: `applications/main/bus_tools/bus_tools_nfc_view.h`
- Modify: `applications/main/bus_tools/scenes/bus_tools_scene_config.h`
- Modify: `applications/main/bus_tools/bus_tools_app_i.h`
- Modify: `applications/main/bus_tools/bus_tools_app.c`
- Modify: `applications/main/bus_tools/scenes/bus_tools_scene_start.c`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a Bus Tools app with no NFC scene/view and no `bus_tools_stnfc_set_receiver` symbol. No other task depends on this task's output (Task 2 is independent).

- [ ] **Step 1: Confirm nothing outside `bus_tools/` references the NFC symbols**

Run (Grep tool or shell), tree-wide:
```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
grep -rn "bus_tools_nfc_view\|bus_tools_stnfc_set_receiver\|BusToolsViewStNfc\|BusToolsSceneStNfc" applications lib components | grep -v "applications/main/bus_tools/"
```
Expected: **no output** (all references are internal to `bus_tools/`). If anything prints, stop and handle it before deleting.

- [ ] **Step 2: Delete the three NFC files**

```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3/applications/main/bus_tools"
rm scenes/bus_tools_scene_st25r_nfc.c bus_tools_nfc_view.c bus_tools_nfc_view.h
```

- [ ] **Step 3: Remove the scene from the X-macro config**

Edit `applications/main/bus_tools/scenes/bus_tools_scene_config.h` — delete this line:
```c
ADD_SCENE(bus_tools, st25r_nfc, StNfc)
```
(The X-macro in `bus_tools_scene.{h,c}` auto-generates the scene id and handler-table slots from this file; removing the line drops the `StNfc` scene and its `bus_tools_scene_st25r_nfc_on_enter/on_event/on_exit` references, which were supplied by the file deleted in Step 2. No edit to `bus_tools_scene.{h,c}` is needed.)

- [ ] **Step 4: Remove NFC members from the app struct/header**

Edit `applications/main/bus_tools/bus_tools_app_i.h`. Remove each of these:
1. The include (line ~8):
```c
#include "bus_tools_nfc_view.h"
```
2. The view enum entry inside `typedef enum { ... } BusToolsView;`:
```c
    BusToolsViewStNfc,
```
3. The struct field + its comment:
```c
    /* ST25R3916 NFC-A "prove it reads" test. */
    View* nfc_view;
```
4. The role-select declaration + its comment block at the bottom:
```c
/* Select the ST25R NFC scene role before entering it: true = receiver (M5 as
 * poller, pulls a file from a Flipper), false = sender (emulate + serve). */
void bus_tools_stnfc_set_receiver(bool receiver);
```

- [ ] **Step 5: Remove the view registration from the app lifecycle**

Edit `applications/main/bus_tools/bus_tools_app.c`. In `bus_tools_app_alloc()` remove:
```c
    app->nfc_view = bus_tools_nfc_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BusToolsViewStNfc, app->nfc_view);
```
In `bus_tools_app_free()` remove:
```c
    view_dispatcher_remove_view(app->view_dispatcher, BusToolsViewStNfc);
    bus_tools_nfc_view_free(app->nfc_view);
```

- [ ] **Step 6: Remove the two menu items + their routing**

Edit `applications/main/bus_tools/scenes/bus_tools_scene_start.c`.
In `bus_tools_scene_start_on_enter`, remove:
```c
    submenu_add_item(
        app->submenu, "NFC Emulate (ST25R)", 3, bus_tools_scene_start_submenu_callback, app);
    submenu_add_item(
        app->submenu, "NFC Receive (ST25R)", 4, bus_tools_scene_start_submenu_callback, app);
```
In `bus_tools_scene_start_on_event`, remove these two branches:
```c
        } else if(submenu_index == 3) {
            bus_tools_stnfc_set_receiver(false);
            scene_manager_next_scene(scene_manager, BusToolsSceneStNfc);
        } else if(submenu_index == 4) {
            bus_tools_stnfc_set_receiver(true);
            scene_manager_next_scene(scene_manager, BusToolsSceneStNfc);
        }
```
Leave the I2C (0), UART (1), and hidden SPI (2) items/branches exactly as they are.

- [ ] **Step 7: Build the firmware — verify clean (this is the test)**

Run (PowerShell tool):
```
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Then verify:
```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
tail -3 build_m5stick_s3.log            # expect ...SENTINEL_OK
grep -iE "bus_tools.*(error|undefined)|error:.*bus_tools" build_m5stick_s3.log   # expect empty
```
Expected: `SENTINEL_OK`, no bus_tools errors. A link error naming `bus_tools_nfc_view_*`, `BusToolsViewStNfc`, or `bus_tools_stnfc_set_receiver` means a reference was missed in Steps 3–6 — fix and rebuild.

- [ ] **Step 8: Commit**

```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
git add -A applications/main/bus_tools
git commit -m "refactor(bus_tools): remove ST25R NFC proof-of-concept scene (superseded by nfc_share app)"
```

---

### Task 2: Show the saved path on a successful receive

Add a `dest_name` field to the engine status, fill it with the actual dedup-resolved filename when a receive target is opened, and render a "Saved to SD" confirmation in the progress view.

**Files:**
- Modify: `applications/main/nfc_share/nfc_share_engine.h` (add `dest_name` to `NfcShareStatus`)
- Modify: `applications/main/nfc_share/nfc_share_engine.c` (fill `dest_name` at receive-target resolve)
- Modify: `applications/main/nfc_share/views/nfc_share_progress_view.c` (render saved name + "Saved to SD")

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `NfcShareStatus.dest_name` (`char[40]`) — the leaf name the received file was actually written as (post-dedup). The progress view reads it; no other task depends on it.

- [ ] **Step 1: Add the `dest_name` field**

Edit `applications/main/nfc_share/nfc_share_engine.h`. In `typedef struct { ... } NfcShareStatus;`, add `dest_name` right after `name`:
```c
typedef struct {
    NfcShareState state;
    char     name[40];
    char     dest_name[40];     // receive: actual saved leaf (post-dedup); empty on send
    uint32_t file_size;
    uint32_t bytes_done;
    uint32_t blocks_total;
    uint32_t blocks_done;
    uint32_t kbps_x10;
    uint8_t  err;               // 0 none, 1 io, 2 md5, 3 radio, 4 toobig
} NfcShareStatus;
```

- [ ] **Step 2: Fill `dest_name` when the receive target is resolved**

Edit `applications/main/nfc_share/nfc_share_engine.c`. In the ANNOUNCE handler, inside the status-mutex block that sets `e->st.*` (immediately after the two lines that copy `leaf` into `e->st.name`):
```c
        strncpy(e->st.name, leaf, sizeof(e->st.name) - 1);
        e->st.name[sizeof(e->st.name) - 1] = '\0';
```
add:
```c
        /* Actual saved leaf (post-dedup) = basename of the resolved dest path. */
        const char* dleaf = strrchr(e->dest, '/');
        dleaf = dleaf ? dleaf + 1 : e->dest;
        strncpy(e->st.dest_name, dleaf, sizeof(e->st.dest_name) - 1);
        e->st.dest_name[sizeof(e->st.dest_name) - 1] = '\0';
```
(`e->dest` was already set by `resolve_dest(...)` earlier in the same block to `/ext/nfc_share/<leaf|_k>`. The send path never enters this block, so `dest_name` stays zero-initialized for sends.)

- [ ] **Step 3: Render the saved name + "Saved to SD" in the progress view**

Edit `applications/main/nfc_share/views/nfc_share_progress_view.c`.

(a) Replace the filename line (currently `canvas_draw_str(canvas, 2, 22, m->status.name[0] ? m->status.name : "-");`) with a version that shows the actual saved name on receive:
```c
    if(!m->sending && m->status.dest_name[0]) {
        char name_line[40];
        snprintf(name_line, sizeof(name_line), "nfc_share/%s", m->status.dest_name);
        canvas_draw_str(canvas, 2, 22, name_line);
    } else {
        canvas_draw_str(canvas, 2, 22, m->status.name[0] ? m->status.name : "-");
    }
```

(b) Change the DoneOk status line so receive reports the SD save (send keeps its text). Replace:
```c
    if(m->status.state == NfcShareStateDoneOk) {
        status_line = "Done - md5 OK";
    } else if(m->status.state == NfcShareStateFail) {
```
with:
```c
    if(m->status.state == NfcShareStateDoneOk) {
        status_line = m->sending ? "Done - md5 OK" : "Saved to SD - md5 OK";
    } else if(m->status.state == NfcShareStateFail) {
```
(Result on a successful receive: line 22 shows `nfc_share/<name>`, bottom line shows `Saved to SD - md5 OK` — together they confirm the file is at `/ext/nfc_share/<name>`.)

- [ ] **Step 4: Build the firmware — verify clean (this is the test)**

Run (PowerShell tool):
```
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Verify:
```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
tail -3 build_m5stick_s3.log            # expect ...SENTINEL_OK
grep -iE "nfc_share.*(error|warning)" build_m5stick_s3.log   # expect empty
```
Expected: `SENTINEL_OK`, no nfc_share errors.

- [ ] **Step 5: Commit**

```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
git add applications/main/nfc_share/nfc_share_engine.h applications/main/nfc_share/nfc_share_engine.c applications/main/nfc_share/views/nfc_share_progress_view.c
git commit -m "feat(nfc_share): show saved filename + 'Saved to SD' on successful receive"
```

---

### Task 3: Mirror all edits into the cardputer tree + build both

**Files:**
- Delete (cardputer): the same three files as Task 1.
- Copy (m5→cardputer): the four edited bus_tools files + the three edited nfc_share files.

**Interfaces:** none new.

- [ ] **Step 1: Confirm the two trees matched before these edits**

The apps were byte-identical prior to this work; verify only the files we changed now differ (m5stick ahead), nothing else drifted:
```bash
M="C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
C="C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_cardputer_adv"
diff -rq "$M/applications/main/bus_tools" "$C/applications/main/bus_tools"
diff -rq "$M/applications/main/nfc_share" "$C/applications/main/nfc_share"
```
Expected: differences ONLY in the files touched by Tasks 1–2 (the 3 deleted bus_tools files show as "Only in .../src_cardputer_adv", and the 4+3 edited files show as "differ"). If any OTHER file differs, investigate before mirroring.

- [ ] **Step 2: Apply the deletions + copies to cardputer**

```bash
M="C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
C="C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_cardputer_adv"
# Task 1 deletions
rm "$C/applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c" \
   "$C/applications/main/bus_tools/bus_tools_nfc_view.c" \
   "$C/applications/main/bus_tools/bus_tools_nfc_view.h"
# Task 1 edited files
cp "$M/applications/main/bus_tools/scenes/bus_tools_scene_config.h" "$C/applications/main/bus_tools/scenes/bus_tools_scene_config.h"
cp "$M/applications/main/bus_tools/bus_tools_app_i.h"               "$C/applications/main/bus_tools/bus_tools_app_i.h"
cp "$M/applications/main/bus_tools/bus_tools_app.c"                 "$C/applications/main/bus_tools/bus_tools_app.c"
cp "$M/applications/main/bus_tools/scenes/bus_tools_scene_start.c"  "$C/applications/main/bus_tools/scenes/bus_tools_scene_start.c"
# Task 2 edited files
cp "$M/applications/main/nfc_share/nfc_share_engine.h"                 "$C/applications/main/nfc_share/nfc_share_engine.h"
cp "$M/applications/main/nfc_share/nfc_share_engine.c"                 "$C/applications/main/nfc_share/nfc_share_engine.c"
cp "$M/applications/main/nfc_share/views/nfc_share_progress_view.c"    "$C/applications/main/nfc_share/views/nfc_share_progress_view.c"
```

- [ ] **Step 3: Verify full parity now**

```bash
diff -rq "$M/applications/main/bus_tools" "$C/applications/main/bus_tools"   # expect: no output
diff -rq "$M/applications/main/nfc_share" "$C/applications/main/nfc_share"   # expect: no output
```
Expected: **no output** from both (trees identical again).

- [ ] **Step 4: Build both trees**

m5stick (PowerShell) — confirms nothing regressed after mirroring:
```
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
cardputer (PowerShell, background):
```
cmd /c '"C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_cardputer_adv\build_adv.bat" < NUL > build_adv.log 2>&1'
```
Verify:
```bash
tail -3 "$M/build_m5stick_s3.log"                                    # expect ...SENTINEL_OK
grep -E "=== SUCCESS ===|=== BUILD FAILED|ninja failed" "$C/build_adv.log"   # expect === SUCCESS ===
ls -la "$C/Flipper-cardputer_adv-merged.bin"                         # fresh timestamp
```
Expected: m5stick `SENTINEL_OK`; cardputer `=== SUCCESS ===` with a fresh merged bin.

- [ ] **Step 5: Commit (m5stick only — cardputer isn't git-tracked)**

The cardputer copies live outside git; the m5stick commits from Tasks 1–2 already capture the source of truth. No extra commit needed here unless Step 4 required a fixup — in which case:
```bash
cd "$M" && git add -A applications/main/bus_tools applications/main/nfc_share && git commit -m "fixup(nfc_share): build fixes from cardputer mirror"
```

---

### Task 4: Flash m5stick + on-device acceptance

**Files:** none.

**Interfaces:** none.

- [ ] **Step 1: Flash the merged bin**

Run (PowerShell tool):
```
cd "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3"; python -m esptool --chip esp32s3 --port COM10 --baud 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 Flipper-m5stick_s3-merged.bin
```
Expected: `Hash of data verified.` + hard reset.

- [ ] **Step 2: Verify the consolidation on-device**

Open **Bus Tools**: confirm the menu no longer lists "NFC Emulate (ST25R)" or "NFC Receive (ST25R)" (only I2C Scanner + UART Console remain). Open the apps menu: confirm **NFC Share** is present.

- [ ] **Step 3: Receive acceptance — file actually lands on SD**

On the M5: **NFC Share → Receive a file**. On a real Flipper (or a second M5): Flipper Share **Send** a multi-block file. Tap the Unit NFC antenna to the sender.
Expected on the M5 screen: progress climbs, line 22 shows `nfc_share/<name>`, bottom line shows **`Saved to SD - md5 OK`**.
Then confirm persistence: pull `/ext/nfc_share/<name>` (WebUI / `storage`) and byte/md5-compare to the source. The file MUST be present and identical.

- [ ] **Step 4: (Optional) cardputer** — when that board + a Unit NFC are available, flash `Flipper-cardputer_adv-merged.bin` (COM12) and repeat Steps 2–3.

---

## Self-Review

**Spec coverage:**
- "Remove Bus Tools NFC scenes entirely (full removal)" → Task 1 (delete 3, edit 4, tree-wide reference check). ✅
- "Show the saved path on a successful receive" → Task 2 (`dest_name` field + fill + view line). ✅
- "Both trees build clean; mirror" → Task 3. ✅
- "On-device receive test confirming the file is on `/ext/nfc_share/`" → Task 4 Step 3. ✅
- "Bus Tools' other scenes untouched" → Task 1 Step 6 leaves I2C/UART/SPI branches intact. ✅
- Unchanged protocol/engine/send/file-browser → no task edits those. ✅

**Placeholder scan:** No TBD/TODO/"handle edge cases". Every edit shows exact code to add/remove. Build steps have concrete verify commands + expected output.

**Type consistency:** `NfcShareStatus.dest_name` is `char[40]`, defined in Task 2 Step 1, filled in Step 2 (`strncpy` with `sizeof - 1`), read in Step 3 (`m->status.dest_name`). `e->dest` is the pre-existing engine field (`char dest[NFC_SHARE_PATH_MAX]`) set by `resolve_dest`. Removed symbols (`bus_tools_nfc_view_alloc/free`, `BusToolsViewStNfc`, `BusToolsSceneStNfc`, `bus_tools_stnfc_set_receiver`) are deleted at every site enumerated by the grep in Task 1 Step 1.

**Open verification (flagged, not blockers):**
- Task 1 Step 1's grep must be empty before deleting — the plan makes that a hard gate.
- Line 22 width: `nfc_share/` (10) + up to 36-char leaf can exceed the 128px line; `canvas_draw_str` clips at the screen edge (filename tail may be cut for very long names). Acceptable — the bottom "Saved to SD - md5 OK" line still confirms the save. If undesirable, drop the `nfc_share/` prefix in Task 2 Step 3(a).
