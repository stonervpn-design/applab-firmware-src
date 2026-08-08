# Resumable Brute Sessions (non-file slice) — Design

**Date:** 2026-08-08
**Branch:** feat/keeloq-no-desync-clone (superset src_m5stick_s3)
**Status:** design → plan → execute inline
**Guardrail:** PRIVATE-only (`#ifndef PUBLIC_BUILD` at every site); public build must stay byte-identical + gc the new symbols. See [[fzcard-private-only-guardrail]].

## Goal
On-device, host-less **resume** of a long fixed-code SubBrute sweep: a sweep survives power-off / app-exit and continues from the last checkpoint with no phone attached. This is a structural capability PandwaRF Rogue Pro cannot match — its brute state lives in the BLE phone app, so a dropped link loses it.

Honest framing (must shape UI copy): resume is **"from the last checkpoint"**, not literal last-code-on-power-yank; every code TX'd since the last periodic save is re-sent on resume. Practical payoff is limited to long (multi-hour / 24-bit) sweeps — small keyspaces finish in seconds. This slice does NOT make an exhaustive 24-bit sweep practical (~days of continuous TX); it makes a long session survive a routine power-off.

## Scope (this slice)
IN: non-file (built-in preset) attacks — persist + resume `{schema_version, attack enum index, current_step, extra_repeats, opencode, two_bytes}`.
OUT (deferred): `SubBruteAttackLoadFile` (idx 44) resume — bf_load_file frees the loaded path, needs re-persist/serialize (phase 2, MODERATE).
EXCLUDED (never resumable): `SubBruteAttackDeBruijn` (idx 43) — single continuous burst, no step/index.

## Verified anchors (real files)
- Enum: `helpers/subbrute_protocols.h` — DeBruijn = idx **43**, LoadFile = idx **44**, `SubBruteAttackTotalCount` = **45**.
- Step counter: `subbrute_worker_get_step()` / `subbrute_worker_set_step()` (`subbrute_worker.h:74,86`), `SubBruteDevice.current_step` (`subbrute_device.h:52`), both `uint64_t`.
- Persistence template: `helpers/subbrute_settings.c` — `flipper_format_file_alloc → open_always → write_header_cstr → insert_or_update_uint32/hex → close/free`, gated on `storage_sd_status()==FSE_OK`; load mirror with version + `index < SubBruteAttackTotalCount` bounds-check.
- State-file convention: `APP_DATA_PATH("bruteforcer.settings")` → resume file = `APP_DATA_PATH("bruteforcer.resume")`.
- Reconstruction (non-file): `subghz_scene_bf_start.c` — `subbrute_device_attack_set(dev, index, repeats)` + `subbrute_worker_init_default_attack(worker, index, current_step, protocol_info, extra_repeats)`; freq/preset/bits/te all derived from the enum via `subbrute_protocol(index)`.
- Hook points: `subghz_scene_bf_run_attack.c` — `SceneManagerEventTypeTick` (UI/app thread, copies worker step→device every tick) for the throttled checkpoint; `on_exit` for save-on-graceful-exit.
- Guard precedent: `scene_config.h:29` `#ifndef PUBLIC_BUILD ADD_SCENE(...CarKeys/Spectrum/VulnMakes)`; `subbrute_worker.c:51` default-on-private.

## Architecture
- **`helpers/subbrute_resume.{c,h}`** (whole file `#ifndef PUBLIC_BUILD`, or no-op stubs in public):
  - `typedef struct { uint32_t schema_version; uint32_t attack; uint64_t current_step; uint32_t extra_repeats; uint64_t opencode; uint16_t two_bytes; } SubBruteResume;`
  - `bool subbrute_resume_is_valid(const SubBruteResume* r);` — **pure**, host-testable: schema_version == SUBBRUTE_RESUME_SCHEMA, attack < SubBruteAttackTotalCount, attack != DeBruijn, attack != LoadFile. (Kept independent of FlipperFormat so it host-tests with a Python replica + xtensa -fsyntax-only.)
  - `bool subbrute_resume_save(const char* path, const SubBruteResume* r);` — FlipperFormat writer (subbrute_settings.c template), storage-gated.
  - `bool subbrute_resume_load(const char* path, SubBruteResume* out);` — reader; returns false unless present AND `subbrute_resume_is_valid`.
  - `bool subbrute_resume_has(const char* path);` — cheap existence+valid probe for the menu.
  - `void subbrute_resume_clear(const char* path);` — delete on sweep completion so a finished sweep isn't offered.
- **`scenes/subghz_scene_bf_resume.c`** (`#ifndef PUBLIC_BUILD`): restore scene — load record → `subbrute_device_attack_set` + `subbrute_worker_init_default_attack` → seed `current_step` → `scene_manager_next_scene(SubGhzSceneBfSetupAttack)` so the user sees the resumed step and presses Run (TX stays user-confirmed; mirrors bf_start).
- **Save hooks** in the SHARED `subghz_scene_bf_run_attack.c` (each `#ifndef PUBLIC_BUILD`): throttled checkpoint in the Tick handler (every ~5–10 s of wall time, tracked by a tick counter — no Date/RNG), + a final save in `on_exit`; `subbrute_resume_clear` when the sweep reaches completion.
- **Menu:** guarded "Resume Brute" item in `subghz_scene_start.c` (+ `SubmenuIndexResumeBrute` in `subghz_scene_start.h`), shown only when `subbrute_resume_has()` is true; nav handler → `SubGhzSceneBfResume`.
- **Registration:** guarded `ADD_SCENE(subghz, bf_resume, BfResume)` in `scene_config.h`.

## Data flow
Run sweep → every ~5–10 s the Tick handler snapshots `{attack, device.current_step, extra_repeats, opencode, two_bytes}` → `subbrute_resume_save`. On graceful exit → final save. On completion → `subbrute_resume_clear`.
Later, cold boot → Start menu shows "Resume Brute" (because `subbrute_resume_has`) → BfResume loads + validates → reconstructs the attack + seeds step → BfSetupAttack → user presses Run → sweep continues from `current_step`.

## Error handling
- Storage not OK (`storage_sd_status() != FSE_OK`, e.g. a unit flashed without the spoofed /ext FAT): save is a no-op with `FURI_LOG_*` AND the scene surfaces a one-line UI warning rather than silently failing.
- Stale record after an enum reorder: schema_version bump + bounds/kind checks reject it (offer nothing) rather than mis-targeting an attack.
- DeBruijn / LoadFile records: rejected by `subbrute_resume_is_valid` (never written for DeBruijn; LoadFile deferred).

## Testing
- **Host test** `tests/test_subbrute_resume.c` — includes only the pure `subbrute_resume_is_valid` core (guarded so it compiles without FlipperFormat): valid record accepted; bad schema / index≥45 / DeBruijn(43) / LoadFile(44) rejected.
- **Python replica** `tools/subbrute_resume/validate_replica.py` — mirror of the validation predicate; ALL PASS.
- **xtensa** `-fsyntax-only` on subbrute_resume.c.
- **Build gates:** private ELF has `subbrute_resume_*` + `subghz_scene_bf_resume_*`; public ELF has **0** of them (gc'd) + car-free PASS + jammer still 0.

## Non-goals
LoadFile resume, DeBruijn resume, configurable checkpoint cadence (fixed ~5–10 s), any change to TX/airtime, making exhaustive 24-bit brute practical.
