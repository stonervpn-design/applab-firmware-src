# Sub-GHz Toolkit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A "SubGhz Toolkit" menu in the Sub-GHz app with two read-only actions: Protocol Analysis (list registered protocols on screen + export to file) and Export Keystore (dump the loaded plaintext KeeLoq keys to file). Adapted from RocketGods-SubGHz-Toolkit (enclave-decrypt dropped — impossible on this port).

**Architecture:** A menu entry + a toolkit submenu scene + a protocol-list scene, using the existing protocol registry (`subghz_protocol_registry`), keystore (`subghz_keystore_get_data`), storage streams, and widget/popup/submenu modules. Purely additive.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper subghz app. Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Reference (read-only):** `<scratchpad>/rocketgods/subghz_toolkit.c` — the protocol-analysis + keystore-export logic (functions around lines 130-410). Port the registry-walk + file-write structure; DO NOT port the enclave-decrypt path (our `furi_hal_crypto_decrypt` is a stub).
- **Confirmed APIs (use as-is):** `#include <lib/subghz/subghz_protocol_registry.h>` → `extern const SubGhzProtocolRegistry subghz_protocol_registry;`; `subghz_protocol_registry_count(&subghz_protocol_registry)`; `subghz_protocol_registry_get_by_index(&subghz_protocol_registry, i)` → `SubGhzProtocol* { const char* name; SubGhzProtocolType type; SubGhzProtocolFlag flag; }` (type: `SubGhzProtocolTypeStatic/Dynamic/Unknown`). Keystore: `SubGhzKeystore* ks = subghz_environment_get_keystore(subghz_txrx_get_environment(subghz->txrx))`; `SubGhzKeyArray_t* arr = subghz_keystore_get_data(ks)`; `M_EACH(k, *arr, SubGhzKeyArray_t)` → `k->name`(FuriString), `k->key`(uint64_t), `k->type`(uint16_t). Firmware header: use `furi_hal_version_get_model_name()`, `furi_hal_version_get_hw_version()`, `furi_hal_version_get_hw_target()` (git-hash/build-date from RocketGods may not exist — omit if absent).
- **Read-only:** no TX, no decryption, no editing. File exports overwrite (latest snapshot). `storage_simply_mkdir(storage, "/ext/subghz/analysis")` before writing.
- **Additive only:** one new start-menu item + appended enum values + new scenes. Don't alter existing subghz menu items/routing.
- Repo is git on `master`; commit per task. Leave untracked helper scripts alone.

---

### Task 1: SubGhz Toolkit — menu entry + toolkit submenu + protocol-analysis + keystore export

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_toolkit.c` (submenu: 2 actions + keystore-export handler)
- Create: `applications/main/subghz/scenes/subghz_scene_toolkit_protocols.c` (protocol list view + export)
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register both)
- Modify: `applications/main/subghz/scenes/subghz_scene_start.c` (+ `subghz_scene_start.h` enum) — menu entry + route

- [ ] **Step 1: Register scenes.** In `subghz_scene_config.h` add:
  ```c
  ADD_SCENE(subghz, toolkit, Toolkit)
  ADD_SCENE(subghz, toolkit_protocols, ToolkitProtocols)
  ```

- [ ] **Step 2: Menu entry.** In `subghz_scene_start.c` (+ its `.h` enum): append `SubmenuIndexToolkit`; `submenu_add_item(subghz->submenu, "SubGhz Toolkit", SubmenuIndexToolkit, subghz_scene_start_submenu_callback, subghz);` (near "Vehicle Vulns"). Route `SubmenuIndexToolkit` → `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneToolkit)`. Mirror the Vehicle Vulns wiring added earlier.

- [ ] **Step 3: Toolkit submenu scene** `subghz_scene_toolkit.c` (model on `subghz_scene_vuln_makes.c`): `on_enter` → `submenu_reset`; `submenu_set_header(subghz->submenu, "SubGhz Toolkit")`; add items `submenu_add_item(..., "Protocol Analysis", 0, cb, subghz)` and `submenu_add_item(..., "Export Keystore", 1, cb, subghz)`; switch to `SubGhzViewIdMenu`. `on_event`: handle a custom event per selection:
  - index 0 (Protocol Analysis) → `scene_manager_next_scene(SubGhzSceneToolkitProtocols)`.
  - index 1 (Export Keystore) → call a local `toolkit_export_keystore(subghz)` (below), then show a popup: `popup_reset(subghz->popup)`; set header "Success!"/"Error!" + text with the path `/ext/subghz/analysis/keeloq_keys.txt`; `popup_set_timeout`/`popup_set_callback` to return; `view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup)` (confirm popup view id in `subghz_i.h`). Popup Back/timeout → `scene_manager_previous_scene` or a custom event back to this scene.
  - `toolkit_export_keystore`: open `RECORD_STORAGE`, `storage_simply_mkdir("/ext/subghz/analysis")`, open a stream to `/ext/subghz/analysis/keeloq_keys.txt` (`FSOM_CREATE_ALWAYS`), write a header + `Total Keys: <count>` + for each key `Manufacturer: %s\nKey (Hex): %016llX\nType: %hu\n\n` (iterate `subghz_keystore_get_data`). Return bool success. (Reference RocketGods lines ~150-210 for the format; adapt to our keystore accessor.)
  - `on_exit`: `submenu_reset`.

- [ ] **Step 4: Protocol-analysis scene** `subghz_scene_toolkit_protocols.c` (model on `subghz_scene_vuln_list.c` for the widget text-scroll): `on_enter`:
  - Build a `FuriString* body = furi_string_alloc()`; `size_t n = subghz_protocol_registry_count(&subghz_protocol_registry)`; for `i` in `0..n`: `const SubGhzProtocol* p = subghz_protocol_registry_get_by_index(&subghz_protocol_registry, i)`; if `p && p->name`, append `"%s\n  %s  flag=0x%08lX\n\n"` with name, type-string (Static/Dynamic/Unknown), `(uint32_t)p->flag`.
  - Also write the same to `/ext/subghz/analysis/protocol_analysis.txt` (mkdir + CREATE_ALWAYS stream): a firmware header (`Model: %s`, `HW ver: %u`, `Target: %u` from `furi_hal_version_*`), `Total Protocols: %zu`, then the per-protocol blocks.
  - Prepend/append a line to the on-screen body noting `Exported: /ext/subghz/analysis/protocol_analysis.txt`.
  - `widget_reset(subghz->widget)`; `widget_add_text_scroll_element(subghz->widget, 0, 0, 128, 64, furi_string_get_cstr(body))`; switch to `SubGhzViewIdWidget`; free `body` after (text-scroll copies the string — same as vuln_list). `on_event` return false; `on_exit` `widget_reset`.

- [ ] **Step 5: Build** → `build_tk1.log`, expect `=== BUILD OK ===`, EXIT=0. Fix API/view-id names by reading `subghz_i.h`, `subghz_scene_vuln_*.c`, `registry.h`, `subghz_keystore.h`, `furi_hal_version.h`. Grep the subghz dir after to confirm existing menu entries intact + no dangling refs.

- [ ] **Step 6: Commit**
```bash
git add applications/main/subghz/
git commit -m "feat(subghz): SubGhz Toolkit — protocol analysis + keystore export (adapted from RocketGods)"
```

---

### Task 2: Docs — test checklist + README note

**Files:**
- Create: `docs/SUBGHZ_TOOLKIT_TEST.md`
- Modify: `README.md`

- [ ] **Step 1: Checklist** `docs/SUBGHZ_TOOLKIT_TEST.md`: Sub-GHz → SubGhz Toolkit → Protocol Analysis lists the registered protocols on screen (scrolls) + writes `/ext/subghz/analysis/protocol_analysis.txt` (verify file on SD with firmware header + all protocols); Export Keystore writes `/ext/subghz/analysis/keeloq_keys.txt` with the 68 manufacturers + shows the success popup with the path; error popup on failure; Back navigates cleanly; no existing Sub-GHz item changed. Mark "not yet hardware-validated — requires physical Cardputer ADV."
- [ ] **Step 2: README note** under Sub-GHz: SubGhz Toolkit — list/export the supported protocol registry + export the loaded KeeLoq keystore to `/ext/subghz/analysis/` (adapted from RocketGods; enclave-decrypt not applicable on this port).
- [ ] **Step 3: Commit**
```bash
git add docs/SUBGHZ_TOOLKIT_TEST.md README.md
git commit -m "docs: subghz toolkit test checklist + README note"
```

---

## Self-Review

**Spec coverage:** Protocol Analysis (view+export) + Export Keystore + menu entry → Task 1; docs → Task 2. Enclave-decrypt correctly dropped. ✅
**Placeholder scan:** registry/keystore/version APIs are named + confirmed; "reference RocketGods lines" is a read-and-adapt of a named file; view-id confirmations are read-then-adapt against named siblings (vuln scenes). No vague TODOs.
**Type consistency:** `subghz_protocol_registry`, `subghz_protocol_registry_count/_get_by_index`, `SubGhzProtocol.name/type/flag`, keystore `SubGhzKey.name/key/type` used consistently. Scene names from ADD_SCENE macros.
**Ordering:** Task 1 → Task 2.
**Honesty:** GPL-3.0 attribution to RocketGods; only the registry-walk/export structure ported, enclave-decrypt dropped as impossible on this hardware.
