# NFC Share — Consolidate to One App + Confirm-Saving — Design

**Date:** 2026-08-01
**Status:** Design (awaiting user review)

## Goal

Make the standalone **NFC Share** app the single NFC file-transfer entry point — "one app, like the Flipper Zero `flipper_share_nfc`" — by removing the superseded Bus Tools NFC proof-of-concept scenes, and make received-file saving visibly confirmed on screen.

## Background / Why

The feature grew up as two inline scenes in the **Bus Tools** app ("NFC Emulate (ST25R)" and "NFC Receive (ST25R)"), used through Phases 1–3. Phase 4 refactored the proven logic into a standalone **NFC Share** app (`applications/main/nfc_share/`) with an SD-backed engine that **streams received files to `/ext/nfc_share/` and md5-verifies them**. Both now coexist, which is confusing and risky:

- The Bus Tools **receive** scene is **RAM-only — it never saves the received file** (memory: "RAM-only, no SD persist yet"). A user testing through it sees "md5 OK" but nothing lands on storage. This is the observed "stuff doesn't get saved" symptom.
- The standalone app's receive path already saves correctly (`resolve_dest` → `storage_file_open(FSOM_CREATE_ALWAYS)` → per-block `storage_file_write` at offset → reopen read-only → `compute_file_md5` verify → `DoneOk`; keeps the file on md5 mismatch for inspection). Verified in `nfc_share_engine.c:230-357`.

So the fix for "ensure the stuff gets saved" is to route the user to the app that already saves, and to surface where the file landed.

## Scope

**In scope**
1. **Remove the Bus Tools NFC scenes entirely** (full removal, no dead code).
2. **Show the saved path** on a successful receive in the NFC Share progress view.
3. Rebuild both trees; re-flash m5stick; on-device receive test confirming the file is on `/ext/nfc_share/`.

**Out of scope / unchanged**
- The nsh wire protocol, the engine send/receive logic, the send path, the file browser, the scene structure of NFC Share.
- The cardputer mirror mechanism (the Bus Tools removal + the status-field addition mirror over the same way).
- Bus Tools' other scenes (I2C Scanner, UART Console, UART Monitor, SPI Flash Reader stub) — untouched.

## Part 1 — Remove Bus Tools NFC (full removal)

Exact reference map (from grep across `applications/main/bus_tools/`):

**Delete:**
- `scenes/bus_tools_scene_st25r_nfc.c` (the ~520-line inline proof-of-concept: listener worker, poller worker, `bus_tools_stnfc_set_receiver`).
- `bus_tools_nfc_view.c`
- `bus_tools_nfc_view.h`

**Edit:**
- `scenes/bus_tools_scene_config.h` — remove line `ADD_SCENE(bus_tools, st25r_nfc, StNfc)`. (The X-macro auto-drops the generated `StNfc` scene id + handler-table slots; the deleted scene `.c` supplied those handlers, so the two stay consistent.)
- `bus_tools_app_i.h` — remove `#include "bus_tools_nfc_view.h"`, the `BusToolsViewStNfc` enum entry, the `View* nfc_view;` field, and the `bus_tools_stnfc_set_receiver(bool)` declaration + its comment.
- `bus_tools_app.c` — remove the 4 nfc_view lines: `bus_tools_nfc_view_alloc()`, `view_dispatcher_add_view(..., BusToolsViewStNfc, ...)`, `view_dispatcher_remove_view(..., BusToolsViewStNfc)`, `bus_tools_nfc_view_free(...)`.
- `scenes/bus_tools_scene_start.c` — remove the two `submenu_add_item(... "NFC Emulate (ST25R)" ...)` / `"NFC Receive (ST25R)"` calls and the two `else if(submenu_index == 3 / == 4)` branches. Leave the I2C/UART items and the already-hidden SPI index-2 branch as-is.

**Result:** Bus Tools reverts to its I2C/UART utility; no unreachable NFC code remains. NFC Share is the sole NFC file-share entry point.

## Part 2 — Confirm-saving UX

The engine already writes + verifies; the only gap is the user can't see *where* the file went (and dedup may have renamed it, e.g. `name_1.ext`).

- Add a `char dest_name[40]` field to `NfcShareStatus` (in `nfc_share_engine.h`). The receive path already computes the final deduped leaf into `e->dest`; copy that leaf into `st.dest_name` under the status mutex when the destination is resolved.
- In `views/nfc_share_progress_view.c`, on `NfcShareStateDoneOk` **for the receive role**, render a line: `Saved: /ext/nfc_share/<dest_name>` (truncate to fit width). Send role keeps its existing "Done – md5 OK".
- No change to send-side behavior or to the `NfcShareStatus` fields the send path uses.

This field addition is in the shared engine header, so it mirrors to `src_cardputer_adv` with the rest.

## Data flow (unchanged except the new field)

Receive: poller worker parses ANNOUNCE → `resolve_dest` picks `/ext/nfc_share/<leaf|_k>` → **copy deduped leaf into `st.dest_name`** → stream DATA blocks to disk → md5-verify from disk → `DoneOk`. Progress scene tick reads `NfcShareStatus` and the view renders the saved path on success.

## Error handling (unchanged)

Engine `err` codes stay: 1=I/O, 2=md5 mismatch, 3=radio, 4=too big/empty. On md5 mismatch the received file is left on disk. Adding `dest_name` doesn't alter any error path.

## Testing

- **Host:** `nfc_share_proto` unit test still passes (unaffected — no protocol change).
- **Build:** both trees compile clean (`build_m5stick_s3.bat` → `SENTINEL_OK`; cardputer `build_adv.bat` → `=== SUCCESS ===`). Confirms the Bus Tools removal left no dangling references and the new struct field mirrors cleanly.
- **On-device (m5stick, the acceptance):** flash; **NFC Share → Receive a file**; a real Flipper (or second M5) sends a multi-block file. Expect progress → `Done`, the screen shows `Saved: /ext/nfc_share/<name>`, and the file is actually present on `/ext/nfc_share/` (pull via WebUI / storage and byte/md5 compare). Confirm Bus Tools no longer lists the two NFC items.

## Files touched (summary)

| Action | File |
|---|---|
| delete | `applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c` |
| delete | `applications/main/bus_tools/bus_tools_nfc_view.c` |
| delete | `applications/main/bus_tools/bus_tools_nfc_view.h` |
| edit | `applications/main/bus_tools/scenes/bus_tools_scene_config.h` |
| edit | `applications/main/bus_tools/bus_tools_app_i.h` |
| edit | `applications/main/bus_tools/bus_tools_app.c` |
| edit | `applications/main/bus_tools/scenes/bus_tools_scene_start.c` |
| edit | `applications/main/nfc_share/nfc_share_engine.h` (add `dest_name`) |
| edit | `applications/main/nfc_share/nfc_share_engine.c` (fill `dest_name`) |
| edit | `applications/main/nfc_share/views/nfc_share_progress_view.c` (show saved path) |
| mirror | all of the above into `src_cardputer_adv` |
