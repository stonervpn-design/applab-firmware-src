# Resumable Brute Sessions (non-file slice) — Implementation Plan

**Goal:** on-device host-less resume of a long fixed-code SubBrute sweep from the last checkpoint. PRIVATE-only.

**Architecture:** pure record + validator (host-tested) → FlipperFormat save/load glue (subbrute_settings.c template) → restore scene → throttled save hooks in bf_run_attack → guarded menu/registration. See spec `2026-08-08-resumable-brute-design.md`.

## Global Constraints
- Every new site `#ifndef PUBLIC_BUILD`; public build byte-identical + new symbols gc'd (verify with nm). Car-free gate must still PASS; jammer must stay 0 in public.
- Record: `{uint32 schema_version, uint32 attack, uint64 current_step, uint8 extra_repeats, uint8 two_bytes, uint8 opencode}`. `SUBBRUTE_RESUME_SCHEMA = 1`.
- Enum anchors (verified): DeBruijn=43, LoadFile=44, `SubBruteAttackTotalCount`=45.
- Resume file: `APP_DATA_PATH("bruteforcer.resume")`. Storage-gated on `storage_sd_status()==FSE_OK`.
- No `Date.now`/RNG anywhere; checkpoint cadence tracked by a tick counter.

---

### Task 1: Pure record + validator + host test
**Files:** Create `helpers/subbrute_resume.h`, `helpers/subbrute_resume.c` (validator only this task), `tests/test_subbrute_resume.c`, `tools/subbrute_resume/validate_replica.py`.
**Produces:** `SubBruteResume` struct; `bool subbrute_resume_is_valid(const SubBruteResume*)` — schema==1 AND attack<45 AND attack!=43 AND attack!=44.
- [ ] Header: struct + `SUBBRUTE_RESUME_SCHEMA` + decls (save/load/has/clear/is_valid), whole thing `#ifndef PUBLIC_BUILD`.
- [ ] `.c`: implement `subbrute_resume_is_valid` using the enum constants.
- [ ] Host test: valid record accepted; bad schema, attack=45/99, attack=43(DeBruijn), attack=44(LoadFile) all rejected. Include a tiny shim for the 3 enum constants if subbrute_protocols.h isn't host-clean.
- [ ] Python replica of the predicate; run → ALL PASS.
- [ ] `xtensa -fsyntax-only` on subbrute_resume.c.

### Task 2: FlipperFormat save / load / has / clear
**Files:** Modify `helpers/subbrute_resume.c`.
**Consumes:** the subbrute_settings.c template. **Produces:** `subbrute_resume_save/load/has/clear`.
- [ ] `save(path, r)`: file_alloc→open_always→write_header_cstr("Flipper SubBrute Resume", 1)→insert_or_update uint32(schema/attack/extra_repeats/two_bytes/opencode) + hex/uint64(current_step)→close/free; storage-gated; return false on any failure.
- [ ] `load(path, out)`: open_existing→read all fields→`subbrute_resume_is_valid`; false unless present AND valid.
- [ ] `has(path)`: existence + valid probe (cheap).
- [ ] `clear(path)`: `storage_simply_remove`.
- [ ] Build private (subbrute app) to confirm it compiles + links.

### Task 3: Restore scene + menu + registration
**Files:** Create `scenes/subghz_scene_bf_resume.c`. Modify `scenes/subghz_scene_config.h`, `scenes/subghz_scene_start.c`, `scenes/subghz_scene_start.h`, `subghz_i.h` (resume-file path const if needed).
**Consumes:** bf_start reconstruction template. **Produces:** `SubGhzSceneBfResume` + "Resume Brute" menu entry.
- [ ] `subghz_scene_bf_resume_on_enter`: `subbrute_resume_load` → on fail show warning widget + back; on ok `subbrute_device_attack_set(dev, r.attack, r.extra_repeats)`, set `dev->two_bytes/opencode/current_step`, `subbrute_worker_init_default_attack(worker, r.attack, r.current_step, dev->protocol_info, r.extra_repeats)`, then `scene_manager_next_scene(SubGhzSceneBfSetupAttack)`.
- [ ] `#ifndef PUBLIC_BUILD ADD_SCENE(subghz, bf_resume, BfResume)`.
- [ ] Guarded "Resume Brute" submenu item (only if `subbrute_resume_has`) + `SubmenuIndexResumeBrute` in start.h + nav handler in start.c — all `#ifndef PUBLIC_BUILD` (like the jammer fix pattern: menu item, handler, ADD_SCENE).

### Task 4: Save hooks in bf_run_attack
**Files:** Modify `scenes/subghz_scene_bf_run_attack.c`.
- [ ] Tick handler: increment a tick counter; every ~N ticks (~5–10 s) snapshot `{schema, attack, device->current_step, extra_repeats, two_bytes, opencode}` → `subbrute_resume_save`. All `#ifndef PUBLIC_BUILD`.
- [ ] `on_exit`: final `subbrute_resume_save`.
- [ ] On sweep completion (worker state finished): `subbrute_resume_clear` so a finished sweep isn't offered.
- [ ] If storage not OK: `FURI_LOG_*` + (scene-side) a one-line UI warning.

### Task 5: Build gates
- [ ] Private build: `nm` shows `subbrute_resume_*` + `subghz_scene_bf_resume_*` PRESENT; host test + replica pass.
- [ ] Public gate: `nm` shows **0** resume symbols; car-free PASS; jammer still 0. Public byte-stable.
- [ ] Commit P5-style (stage only the feature files).
