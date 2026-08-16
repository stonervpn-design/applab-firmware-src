# Asset Packs (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Swappable Asset Packs, Phase 1 — a user-selectable pack under `/ext/asset_packs/<name>/` supplies the dolphin **animations** (and optional **boot logo**); selection lives in Settings; a missing/partial pack falls back to the built-in assets and never crashes. (Phase 2 = icon/font override — a separate later plan.)

**Architecture:** Turn the hardcoded animation base dir (`ANIMATION_DIR = /ext/dolphin`) into a runtime resolver that returns the active pack's `Anims/` dir (or the default `/ext/dolphin`) based on a new `DesktopSettings.asset_pack` name. A pure path-resolver + pack-enumeration helper is host-tested. A settings selector enumerates `/ext/asset_packs/*` and stores the choice. The boot splash loads the pack's logo if present.

**Tech Stack:** C (ESP-IDF Flipper port), desktop animation service + desktop_settings, storage API, WSL gcc for the host test.

**Spec:** `docs/superpowers/specs/2026-08-16-port-ux-security-pack-design.md` (Feature C, Phase 1)

## Global Constraints
- Superset `src_m5stick_s3` only; mirror deferred to the combined end.
- UX feature — NOT `PUBLIC_BUILD`-gated.
- **Fail-safe:** any missing/malformed/partial pack (missing manifest, missing frame, bad name) MUST degrade to the built-in `/ext/dolphin` + compiled-in animations. No crash, no blank screen. The default pack name (`""` or `"Default"`) means "use built-in".
- `DesktopSettings` is at migration version V18 (after Feature B's `lock_on_boot`). Adding `asset_pack` requires extending the migration the SAME way Feature B did (bump version, default the new field on older loads). Grep the current `desktop_settings.{c,h}` to match the exact pattern.
- Commit discipline: unrelated changes exist (nrf24/kia/DESFire/level/security); `git add` ONLY this plan's explicit paths, never `git add -A`. Heredoc/-F messages. Trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` / `Claude-Session: https://claude.ai/code/session_01G1b3g8F3qEPrP6Esy5nYHq`.
- Build via PowerShell + `build_m5stick_s3.bat`; stale-log gotcha (delete dir, fresh-log verify SENTINEL_OK, 0 errors, fresh binary); after launching a build, actively wait for + verify SENTINEL_OK yourself.

## Grounded facts (verified in `applications/services/desktop/animations/animation_storage.c`)
- `#define ANIMATION_DIR EXT_PATH("dolphin")` (line 15), `ANIMATION_MANIFEST_FILE = ANIMATION_DIR "/manifest.txt"` (line 16). Used at: manifest open (lines 39, 96), frame path `ANIMATION_DIR "/%s/frame_%d.bm"` (line 296), and meta/bubble loads. `animation_storage_fill_animation_list()` (line 80) builds the list from the manifest + compiled-in `dolphin_internal[]`.
- Pack layout: `/ext/asset_packs/<Name>/Anims/manifest.txt` + `/ext/asset_packs/<Name>/Anims/<anim>/frame_*.bm` (mirrors the `/ext/dolphin` layout so the existing loader works unchanged once the base dir is redirected).

---

## Task 1: Asset-pack path resolver + settings field + host test

**Files:**
- Create: `applications/services/desktop/helpers/asset_pack.{c,h}`
- Modify: `applications/services/desktop/desktop_settings.{c,h}` (add `char asset_pack[ASSET_PACK_NAME_LEN];` default `""`, with migration)
- Create: `tools/asset_pack_host_test/{Makefile,test_asset_pack.c}`

**Interfaces (produces):**
- `#define ASSET_PACK_NAME_LEN 32`, `#define ASSET_PACKS_DIR "/ext/asset_packs"`.
- `void asset_pack_anims_dir(const char* pack_name, char* out, size_t out_size);` — PURE: if `pack_name` is empty/NULL/`"Default"`, writes `/ext/dolphin`; else writes `/ext/asset_packs/<pack_name>/Anims`. Bounded (never overflows `out`). Host-testable.
- `bool asset_pack_name_is_valid(const char* name);` — PURE: rejects empty (that's "default", handled separately), names with `/`, `..`, control chars, or length ≥ ASSET_PACK_NAME_LEN. Prevents path injection.
- (firmware) `size_t asset_pack_enumerate(char names[][ASSET_PACK_NAME_LEN], size_t max);` — lists subdirs of `/ext/asset_packs` that contain an `Anims/manifest.txt`; returns count. Firmware-only (storage); host test covers only the pure resolver/validator.

- [ ] **Step 1: host test** (`tools/asset_pack_host_test/`) compiling the pure resolver + validator (put them in a form the test includes without furi). Asserts:
```c
char b[64];
asset_pack_anims_dir("", b, sizeof b);        assert(!strcmp(b, "/ext/dolphin"));
asset_pack_anims_dir(NULL, b, sizeof b);       assert(!strcmp(b, "/ext/dolphin"));
asset_pack_anims_dir("Default", b, sizeof b);  assert(!strcmp(b, "/ext/dolphin"));
asset_pack_anims_dir("Neon", b, sizeof b);     assert(!strcmp(b, "/ext/asset_packs/Neon/Anims"));
assert(asset_pack_name_is_valid("Neon"));
assert(!asset_pack_name_is_valid(""));
assert(!asset_pack_name_is_valid("../x"));
assert(!asset_pack_name_is_valid("a/b"));
assert(!asset_pack_name_is_valid("this_name_is_way_too_long_to_fit_in_the_buffer_limit")); // >= 32
// bounded: tiny buffer must truncate safely, not overflow
char t[8]; asset_pack_anims_dir("Neon", t, sizeof t); assert(t[7] == '\0');
```
- [ ] **Step 2: run → RED.**
- [ ] **Step 3: implement** the resolver (exact, bounded via `snprintf`) + validator; add `asset_pack[ASSET_PACK_NAME_LEN]` to `DesktopSettings` with migration (default `""`); implement `asset_pack_enumerate` (grep the storage dir-iteration API — `storage_dir_open`/`storage_dir_read` or `Storage`/`File` API — use the real one).
- [ ] **Step 4: run → GREEN.**
- [ ] **Step 5: commit.**

---

## Task 2: Redirect animation loading to the active pack (with fallback)

**Files:**
- Modify: `applications/services/desktop/animations/animation_storage.c`

- [ ] **Step 1:** Replace the hardcoded `ANIMATION_DIR`/`ANIMATION_MANIFEST_FILE` usages with a runtime base dir from `asset_pack_anims_dir(active_pack, ...)`. Read the active pack from `DesktopSettings` (grep how animation_storage / animation_manager can reach the desktop settings, or pass the pack name in when the animation manager initializes — the desktop owns both; thread the active pack name from desktop settings into the animation list build). Concretely: compute `base_dir` once, then build `<base_dir>/manifest.txt` and `<base_dir>/%s/frame_%d.bm` from it instead of the macros.
- [ ] **Step 2: Fallback logic:** if the pack's `manifest.txt` is missing or fails to open, OR a referenced frame/meta file is missing, fall back to `/ext/dolphin` and the compiled-in `dolphin_internal[]` (the existing behavior when `/ext/dolphin` is absent). A pack that only partially loads must not blank the idle screen — the existing code already falls back to compiled-in animations when the external manifest is absent; ensure the redirect preserves that path (i.e. an invalid pack behaves exactly like "no external manifest").
- [ ] **Step 3:** Build m5stick_s3 clean (validates Task 1 + Task 2). Fix desktop-animation compile errors only. Actively verify SENTINEL_OK.
- [ ] **Step 4: commit.**

---

## Task 3: Asset-pack selector UI

**Files:**
- Modify: `applications/settings/desktop_settings/scenes/desktop_settings_scene_start.c` (+ a sub-scene if a dedicated list is cleaner)

- [ ] **Step 1:** Add an "Asset Pack" item to the settings list. On select, enumerate `/ext/asset_packs/*` (via `asset_pack_enumerate`) plus a "Default" entry; present them (a variable-item cycling through found packs, or a submenu list if many). Store the chosen name in `settings.asset_pack` and save.
- [ ] **Step 2:** Show the current `settings.asset_pack` as the selected value on entry ("Default" when empty). Validate the chosen name with `asset_pack_name_is_valid` before storing (defense-in-depth; enumerated names are trusted but validate anyway).
- [ ] **Step 3:** Note in-UI (or accept) that an animation change applies on next desktop (re)load — do not require a live reload in Phase 1. Build m5stick_s3 clean; verify SENTINEL_OK.
- [ ] **Step 4: commit.**

---

## Task 4: Boot logo swap + example pack + build

**Files:**
- Modify: the boot-splash code (grep for the boot logo / splash draw — likely in the desktop or a boot scene / `applications/services/desktop` or a startup screen; find where the boot image is drawn).
- Create (staged, not committed to firmware): an example pack under the SD staging dir the port uses for `/ext` (grep the `_extfs_stage*` / `_sdstage` dir used by this tree) — `asset_packs/Example/Anims/manifest.txt` + a couple of frames, so the feature is demonstrable.

- [ ] **Step 1:** Find where the boot splash/logo is rendered. If a selected pack has a `Logo.bm` (or equivalent) under its dir, load+draw that at boot; else the built-in. Keep fail-safe (missing logo → built-in). If boot-logo swapping is architecturally heavy on this port, implement the animation+selection (Tasks 1-3) fully and mark boot-logo as a documented Phase-1.5 deferral in the report — do not force a fragile hack.
- [ ] **Step 2:** Add the example pack to the `/ext` staging dir (grep the tree's staging mechanism). This is SD content, not firmware.
- [ ] **Step 3:** Final Phase-1 build m5stick_s3 clean; verify SENTINEL_OK + fresh binary.
- [ ] **Step 4: commit.**

---

## Self-Review
- Spec coverage (Feature C Phase 1): pack layout + selection (Tasks 1, 3) ✓; animations from pack with fallback (Task 2) ✓; boot logo (Task 4, with documented deferral option) ✓; fail-safe/robustness (Task 2 + the validator/resolver) ✓; example pack (Task 4) ✓. Phase 2 (icon/font override) explicitly out of scope (separate plan).
- Safety: pack name validated (no `/`/`..`/overflow) before it ever forms a path; resolver is bounded (`snprintf`); host test covers default/pack/injection/truncation.
- No placeholders: the base-dir redirect points are concrete (animation_storage.c lines 39/96/296); the storage dir-iteration + boot-splash APIs are grep-verified by the implementer; settings migration matches Feature B's established pattern.
- Fallback-to-built-in is the central robustness requirement and is called out in Task 2.
