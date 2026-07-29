# Vehicle Vulnerability (CVE) Lookup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A read-only "Vehicle Vulns" browser in the Sub-GHz app: make → per-make list of known keyless-entry CVEs (years, CVE id, description), ported from KAT's `vuln_db.rs`.

**Architecture:** A C data table (`vehicle_vuln_db`) + two subghz scenes (make picker submenu, per-make CVE list via widget text-scroll) reachable from the Sub-GHz start menu. Purely additive.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper subghz app (submenu, widget). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Source (read-only):** `C:\Users\stone\Downloads\KAT-main\KAT-main\src\vuln_db.rs` — the `VULN_DB` array (21 entries). Transcribe all 21 VERBATIM (cve, year_start, year_end, makes, models, region, description, url). Public CVE/NVD data — copy exactly, do not paraphrase descriptions.
- **Read-only feature:** no TX, no capture matching, no editing. Just browse the static table.
- **Additive only:** the Sub-GHz start menu gains one item; new scenes + enum values are appended. Do not alter existing subghz menu entries/routing.
- Repo is git on `master`; commit per task. Leave untracked helper scripts alone.

---

### Task 1: `vehicle_vuln_db.{c,h}` — port the 21-entry CVE table

**Files:**
- Create: `applications/main/subghz/helpers/vehicle_vuln_db.h`
- Create: `applications/main/subghz/helpers/vehicle_vuln_db.c`

**Interfaces (produce):**
```c
typedef struct {
    const char*        cve;
    const char*        year_start;   // "ALL" allowed
    const char*        year_end;     // "ALL" allowed
    const char* const* makes;        // NULL-terminated array
    const char* const* models;       // NULL-terminated array
    const char*        region;
    const char*        description;
    const char*        url;
} VehicleVuln;

extern const VehicleVuln VEHICLE_VULN_DB[];
extern const size_t VEHICLE_VULN_DB_COUNT;

size_t vehicle_vuln_make_count(void);          // distinct makes across the DB
const char* vehicle_vuln_make(size_t i);       // i-th distinct make
```

- [ ] **Step 1: Header** — `vehicle_vuln_db.h`: `#pragma once`, `extern "C"` guard, `#include <stddef.h>`, the struct + the four declarations above.

- [ ] **Step 2: Data** — `vehicle_vuln_db.c`: READ `KAT/src/vuln_db.rs` and transcribe ALL 21 `VULN_DB` entries into `const VehicleVuln VEHICLE_VULN_DB[]`. For each Rust `makes: &["Renault"]` / `models: &["Cerato","Forte","K3"]`, define a `static const char* const <name>_makes[] = {"Renault", NULL};` (NULL-terminated) and reference it. Set `VEHICLE_VULN_DB_COUNT = sizeof(...)/sizeof(...[0])`. Copy `description`/`cve`/`url`/years/region verbatim.

- [ ] **Step 3: Make accessors** — implement `vehicle_vuln_make_count`/`vehicle_vuln_make` by scanning all entries' `makes[]` and returning DISTINCT make strings (skip "ALL"; dedupe case-sensitively). A simple static-cached distinct-list built on first call, or an O(n) scan each call (21 entries — trivial either way).

- [ ] **Step 4: Build** → `build_vv1.log`, expect `=== BUILD OK ===`, EXIT=0. (Unused so far; proves it compiles/links.)

- [ ] **Step 5: Commit**
```bash
git add applications/main/subghz/helpers/vehicle_vuln_db.h applications/main/subghz/helpers/vehicle_vuln_db.c
git commit -m "feat(subghz): vehicle vulnerability CVE database (ported from KAT)"
```

---

### Task 2: Sub-GHz UI — make picker + per-make CVE list + menu entry

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_vuln_makes.c`
- Create: `applications/main/subghz/scenes/subghz_scene_vuln_list.c`
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register both)
- Modify: `applications/main/subghz/scenes/subghz_scene_start.c` (menu entry + route)
- Modify: the subghz i-header if a small piece of state is needed (the selected make index) — prefer `scene_manager_set_scene_state` to pass the selected make index instead of adding a struct field.

- [ ] **Step 1: Register scenes.** In `subghz_scene_config.h` add:
  ```c
  ADD_SCENE(subghz, vuln_makes, VulnMakes)
  ADD_SCENE(subghz, vuln_list, VulnList)
  ```

- [ ] **Step 2: Menu entry.** In `subghz_scene_start.c`: add `SubmenuIndexVehicleVulns` to the `SubmenuIndex` enum (append, don't renumber others); `submenu_add_item(subghz->submenu, "Vehicle Vulns", SubmenuIndexVehicleVulns, subghz_scene_start_submenu_callback, subghz);` (place near "Saved"/"Add Manually"). In the submenu callback / on_event, when `SubmenuIndexVehicleVulns` is selected → `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnMakes)`.

- [ ] **Step 3: Make picker scene** `subghz_scene_vuln_makes.c` (model on `subghz_scene_start.c`'s submenu usage): `on_enter` → `submenu_reset(subghz->submenu)`; for `i` in `0..vehicle_vuln_make_count()` → `submenu_add_item(subghz->submenu, vehicle_vuln_make(i), i, callback, subghz)`; set header "Vehicle Vulns"; `view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu)` (confirm the submenu view id name in `subghz_i.h`). Callback: `scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneVulnMakes, index)` then `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnList)`. `on_exit`: `submenu_reset`.

- [ ] **Step 4: Per-make list scene** `subghz_scene_vuln_list.c` (use `subghz->widget` text-scroll — see `views/subghz_read_raw.c` / a scene using `widget_add_text_scroll_element`): `on_enter` → read the selected make via `scene_manager_get_scene_state(..., SubGhzSceneVulnMakes)` → resolve to the make string with `vehicle_vuln_make(idx)`; build one `FuriString` concatenating every `VEHICLE_VULN_DB[k]` whose `makes[]` contains that make, each formatted:
  ```
  <models joined by "/">  <year_start>-<year_end>
  <cve>  (<region>)
  <description>

  ```
  `widget_reset(subghz->widget)`; `widget_add_text_scroll_element(subghz->widget, 0, 0, 128, 64, furi_string_get_cstr(str))`; `view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget)` (confirm id). Free the FuriString in `on_exit` after `widget_reset`. Back → returns to the make picker.

- [ ] **Step 5: Build** → `build_vv2.log`, expect `=== BUILD OK ===`, EXIT=0. Fix view-id / accessor names by reading `subghz_i.h` and the sibling scenes. Grep the whole subghz dir afterward to confirm no dangling references and existing menu entries are intact.

- [ ] **Step 6: Commit**
```bash
git add applications/main/subghz/
git commit -m "feat(subghz): Vehicle Vulns browser (make -> CVE list)"
```

---

### Task 3: Docs — test checklist + README note

**Files:**
- Create: `docs/VEHICLE_VULNS_TEST.md`
- Modify: `README.md`

- [ ] **Step 1: Checklist** `docs/VEHICLE_VULNS_TEST.md`: Sub-GHz → Vehicle Vulns → make picker lists the DB makes (Renault, Honda, Hyundai, Kia, Mazda, …); pick Honda → list shows Civic/Fit/City/Vezel entries with CVE ids, year ranges, descriptions; scroll works; Back returns to picker then to Sub-GHz; no existing Sub-GHz menu item changed. Mark "not yet hardware-validated — requires physical Cardputer ADV."
- [ ] **Step 2: README note** under Sub-GHz: Vehicle Vulns — browse known keyless-entry/RKE CVEs by make/model (replay, RollBack); reference DB (data from NVD via KAT). Note it's a reference lookup, not automatic capture matching.
- [ ] **Step 3: Commit**
```bash
git add docs/VEHICLE_VULNS_TEST.md README.md
git commit -m "docs: vehicle vulns test checklist + README note"
```

---

## Self-Review

**Spec coverage:** DB port → Task 1 (all 21 verbatim); make→CVE-list UI + menu entry → Task 2; docs → Task 3. ✅
**Placeholder scan:** "transcribe all 21 from vuln_db.rs" is a concrete read-and-copy of a named file; view-id confirmations are read-then-adapt against named siblings. No vague TODOs.
**Type consistency:** `VehicleVuln`, `VEHICLE_VULN_DB`, `vehicle_vuln_make*` (Task 1) consumed by Task 2. Scene names from ADD_SCENE macros used in routing.
**Ordering:** Task 1 → Task 2 (UI needs the DB) → Task 3.
**Honesty:** CVE data is public/NVD, copied verbatim from KAT; no KAT RF code used; feature is a browse reference, not RF→CVE auto-matching (our analyzer decodes RF, not vehicle make/model).
