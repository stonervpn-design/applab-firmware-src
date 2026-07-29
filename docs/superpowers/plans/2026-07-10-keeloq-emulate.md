# KeeLoq direct-Emulate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Add a direct "Emulate" route from the KeeLoq by-manufacturer generator into the existing Transmitter/emulator (counter-advance + custom buttons), via an Emulate/Save choice shown only for that flow.

**Architecture:** A flag marks the by-manufacturer path; after its generation, a 2-item action scene routes Emulate→Transmitter or Save→SaveName. Additive; other protocols unchanged.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper subghz app. Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- Model: opus (subghz flow integration; Sonnet cyber-gate).
- **Only the by-manufacturer path may change behavior.** Hardcoded KeeLoq entries and all other Add-Manually protocols must still go straight to SaveName (flag stays false).
- Reuse the Transmitter as-is (no changes to the emulator engine).
- Repo is git on `master`; commit per task.

---

### Task 1: Emulate/Save choice for the by-manufacturer flow

**Files:**
- Modify: `applications/main/subghz/subghz_i.h` (add flag `bool keeloq_mfr_emulate_flow;` to the `SubGhz` struct)
- Modify: `applications/main/subghz/scenes/subghz_scene_keeloq_mfr.c` (set flag on manufacturer selection)
- Modify: `applications/main/subghz/scenes/subghz_scene_set_counter.c` (branch on the flag at generation success)
- Modify: `applications/main/subghz/scenes/subghz_scene_start.c` (defensive reset of the flag on Add-Manually entry — or in set_type on_enter)
- Create: `applications/main/subghz/scenes/subghz_scene_keeloq_action.c`
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register the action scene)

- [ ] **Step 1: Flag.** Add `bool keeloq_mfr_emulate_flow;` to the `SubGhz` struct in `subghz_i.h`. Initialize `false` in subghz app alloc (find where the struct is zero'd/alloc'd; if `malloc`+manual init, set it false explicitly). Reset it `false` defensively when the Add-Manually / set_type start scene is entered (read `subghz_scene_start.c` and/or `subghz_scene_set_type.c` on_enter — set the flag false there so a stale true from a cancelled prior flow can't leak).

- [ ] **Step 2: Set the flag.** In `subghz_scene_keeloq_mfr.c`, in the manufacturer-selection callback (where it copies the manufacturer into `gen_info.keeloq.manuf` and routes to `SubGhzSceneSetSerial`), set `subghz->keeloq_mfr_emulate_flow = true;`. This marks that generation came from the by-manufacturer picker.

- [ ] **Step 3: Register the action scene.** In `subghz_scene_config.h` add `ADD_SCENE(subghz, keeloq_action, KeeloqAction)`.

- [ ] **Step 4: Branch at generation.** In `subghz_scene_set_counter.c`, at the generation-success block that currently does `subghz_file_name_clear(subghz); scene_manager_set_scene_state(..., SubGhzSceneSetType, SubGhzCustomEventManagerSet); scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);` — wrap it:
  ```c
  if(subghz->keeloq_mfr_emulate_flow) {
      scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqAction);
  } else {
      subghz_file_name_clear(subghz);
      scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneSetType, SubGhzCustomEventManagerSet);
      scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);
  }
  ```
  (Do NOT clear the flag here — the action scene clears it, so Back from the action scene keeps it consistent. If a Back-loop issue arises, clear it in the action scene's on_exit instead.)

- [ ] **Step 5: Action scene** `subghz_scene_keeloq_action.c` (model on `subghz_scene_keeloq_mfr.c` submenu structure): `on_enter` → `submenu_reset`; header "KeeLoq Fob"; items "Emulate" (idx 0) and "Save" (idx 1); switch to `SubGhzViewIdMenu`. Callback:
  - idx 0 (Emulate): `subghz->keeloq_mfr_emulate_flow = false;` then `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);`
  - idx 1 (Save): `subghz->keeloq_mfr_emulate_flow = false; subghz_file_name_clear(subghz); scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneSetType, SubGhzCustomEventManagerSet); scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);`
  `on_exit`: `submenu_reset`. `on_event` return false (Back → previous scene; also clear the flag on Back so a back-out doesn't leave it set — set `keeloq_mfr_emulate_flow=false` in on_exit to be safe).

- [ ] **Step 6: Build** → `build_kle1.log`, expect `=== BUILD OK ===`, EXIT=0. Verify: read the Transmitter on_enter to confirm it reads the generated `subghz_txrx_get_fff_data` (so entering it from this path shows/sends the generated fob). If the Transmitter needs the fff data loaded a specific way that generation didn't do, note it — but generation populates the same data SaveName saves, so it should work.

- [ ] **Step 7: Commit**
```bash
git add applications/main/subghz/
git commit -m "feat(subghz): direct Emulate option for KeeLoq by-manufacturer fobs"
```

---

### Task 2: Docs

- [ ] `docs/KEELOQ_EMULATE_TEST.md`: Add Manually → KL: By Manufacturer → pick mfr → serial/button/counter → Emulate/Save choice appears → Emulate → Transmitter (OK sends, counter advances each press; Up/Down/Left/Right = different buttons) → Save → SaveName as before → hardcoded KeeLoq entries + other protocols still go straight to SaveName (no choice). Mark "not yet hardware-validated". README note: KeeLoq by-manufacturer generator now offers direct Emulate (into the counter-advancing transmitter) or Save. Commit `docs: KeeLoq direct-emulate test checklist + README`.

---

## Self-Review

**Spec coverage:** flag + set + branch + action scene (Emulate→Transmitter / Save→SaveName) → Task 1; docs → Task 2. Only by-manufacturer path affected. ✅
**Placeholder scan:** exact branch code given; action scene modeled on a named sibling; flag-reset points named. No vague TODOs.
**Risk:** stale flag leaking to a non-emulate flow → mitigated by reset on Add-Manually/set_type entry + clear in the action scene (on select AND on_exit).
**Ordering:** Task 1 → Task 2.
