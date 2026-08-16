# Security: Lock-on-Boot + Wipe-After-N Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add anti-theft security to the desktop: lock the device on boot when a PIN is set, and after N consecutive wrong PIN entries (default 10, configurable, persisted across reboots) wipe the user-data directories on SD — preserving firmware, asset packs, and system config.

**Architecture:** Extend the existing desktop lock/PIN system. Lock-on-boot hooks desktop init to start in the locked state. A consecutive-wrong-PIN counter is persisted (survives power-off) and incremented at the wrong-PIN path in the locked scene; on reaching the threshold, a well-guarded wipe helper recursively deletes a **whitelisted** set of user-capture directories. All destructive logic is centralized in one helper with a host-tested path-eligibility predicate. Settings expose the toggles + threshold.

**Tech Stack:** C (ESP-IDF Flipper port), the desktop service + desktop_settings app, the storage API for recursive delete, WSL gcc for the host test of the wipe-eligibility predicate.

**Spec:** `docs/superpowers/specs/2026-08-16-port-ux-security-pack-design.md` (Feature B)

## Global Constraints
- Superset `src_m5stick_s3` only; mirror deferred to the combined end.
- UX/security feature — NOT `PUBLIC_BUILD`-gated (present in both builds).
- **Destructive code discipline:** the wipe touches only a `static const char* const` whitelist of user-capture dirs; it must NEVER delete `/ext` root, `/ext/asset_packs`, `/ext/dolphin`, firmware/system, or settings. The path-eligibility check is a pure function with a host test asserting the protected dirs are rejected.
- **Persistence across full power-off is mandatory** for the fail counter (else power-cycling defeats it). Prefer the same mechanism the PIN uses if it survives power-off (the PIN lives in an RTC backup register via `furi_hal_rtc_*`); if that does not survive a battery pull on this board, persist in the desktop settings saved-struct (internal flash). The implementer verifies which persists and states the choice.
- Commit discipline: unrelated changes (nrf24, kia, DESFire, level-system) exist; `git add` ONLY this plan's explicit paths, never `git add -A`. Heredoc/-F messages. Trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` / `Claude-Session: https://claude.ai/code/session_01G1b3g8F3qEPrP6Esy5nYHq`.
- Build via PowerShell + `build_m5stick_s3.bat`; stale-log gotcha (delete dir, fresh-log verify, 0 errors, SENTINEL_OK, fresh binary). After launching a build, ACTIVELY wait for + verify SENTINEL_OK yourself; do not end the turn merely having launched it.

## Grounded facts (verified)
- `applications/services/desktop/helpers/pin_code.c`: `desktop_pin_code_is_set()` (PIN in RTC backup register), `desktop_pin_code_set()`, pack/unpack helpers.
- `applications/services/desktop/desktop.c`: `desktop_lock(desktop)`, `desktop->locked`, `desktop->settings` (DesktopSettings), auto-lock timer system, and existing `desktop_pin_code_is_set()` branches (~lines 718/748/842). Boot/init allocates views (~556-640).
- The wrong-PIN comparison happens in the locked scene / pin-input flow (`applications/services/desktop/scenes/desktop_scene_locked.c` + `views/desktop_view_pin_input.*`). Grep to confirm the exact success/fail branch before editing.
- `DesktopSettings` is a saved struct (settings persisted). Find its definition (grep `typedef struct` + `DesktopSettings` under `applications/services/desktop` / a shared settings header) to add fields.

---

## Task 1: Wipe helper + host-tested path-eligibility predicate

**Files:**
- Create: `applications/services/desktop/helpers/security_wipe.{c,h}`
- Create: `tools/security_wipe_host_test/Makefile`, `test_wipe.c`

**Interfaces:**
- Produces:
  - `bool security_wipe_path_is_eligible(const char* path);` — PURE. Returns true iff `path` exactly matches one of the whitelisted user-capture dirs and is NOT a protected path. No furi/storage deps → host-testable. The whitelist + protected list live here as `static const char* const`.
  - `void security_wipe_user_data(void);` — iterates the whitelist, recursively deleting each eligible dir via the storage API (`furi_record_open(RECORD_STORAGE)`, `storage_simply_remove_recursive` or equivalent — grep the storage API for the real recursive-remove name), logging each. This function is firmware-only (uses storage); guard its body so the host test compiles only the predicate (e.g. keep `security_wipe_user_data` in the .c but the host test includes only the predicate, or split the predicate into a `_pure` section behind no furi includes).

**Whitelist (user-capture dirs to wipe):** `/ext/nfc`, `/ext/subghz`, `/ext/lfrfid`, `/ext/infrared`, `/ext/badusb`, `/ext/ibutton`, `/ext/rfid`, `/ext/u2f`, `/ext/apps_data`. **Protected (never eligible):** `/ext`, `/ext/`, `/ext/asset_packs`, `/ext/dolphin`, `/ext/apps`, `/ext/update`, `/ext/.obj`, any path not exactly in the whitelist.

- [ ] **Step 1: Write the host test** (`tools/security_wipe_host_test/`) compiling only the predicate (put `security_wipe_path_is_eligible` + the two lists in a pure `.c`/`.h` with no furi includes; if `security_wipe.c` must include furi for the wipe function, extract the predicate + lists into the header or a `security_wipe_paths.inc` the test includes). Asserts:
```c
// eligible
assert(security_wipe_path_is_eligible("/ext/nfc"));
assert(security_wipe_path_is_eligible("/ext/subghz"));
assert(security_wipe_path_is_eligible("/ext/badusb"));
// protected / not eligible
assert(!security_wipe_path_is_eligible("/ext"));
assert(!security_wipe_path_is_eligible("/ext/"));
assert(!security_wipe_path_is_eligible("/ext/asset_packs"));
assert(!security_wipe_path_is_eligible("/ext/dolphin"));
assert(!security_wipe_path_is_eligible("/ext/nfc/../.."));   // no traversal match
assert(!security_wipe_path_is_eligible("/"));
assert(!security_wipe_path_is_eligible(""));
assert(!security_wipe_path_is_eligible("/ext/nfcX"));        // exact match only
```
- [ ] **Step 2: Run host test → RED** (`wsl bash -lc "cd tools/security_wipe_host_test && make test"`).
- [ ] **Step 3: Implement** the predicate (exact string match against the whitelist; nothing derived from input beyond exact compare) + `security_wipe_user_data()` (grep the real recursive-remove storage API; iterate the whitelist calling it on each; log each dir).
- [ ] **Step 4: Run host test → GREEN.**
- [ ] **Step 5: Commit** (`security_wipe.{c,h}`, host test).

---

## Task 2: Persisted consecutive-wrong-PIN counter + threshold wipe trigger

**Files:**
- Modify: the `DesktopSettings` struct + its save/load (add `uint8_t pin_fails_before_wipe;` default 10, `bool wipe_enabled;` default true). Add a persisted counter — either a new saved field `uint8_t pin_fail_count;` or an RTC register (per the persistence decision).
- Modify: `applications/services/desktop/scenes/desktop_scene_locked.c` (or the exact wrong-PIN branch found by grep) to increment the counter on a wrong PIN, reset it to 0 on a correct unlock, and call `security_wipe_user_data()` + reset the counter + show a notice when `wipe_enabled && pin_fail_count >= pin_fails_before_wipe`.

**Interfaces:**
- Consumes: `security_wipe_user_data()` (Task 1), the existing PIN-verify success/fail branch.

- [ ] **Step 1:** Grep + read the exact locked-scene / pin-input path where a PIN entry is judged correct vs wrong. Identify the success branch (unlock) and the failure branch (wrong PIN, likely leads to the pin-timeout/lockout view).
- [ ] **Step 2:** Add the settings fields (grep the `DesktopSettings` struct + its version/migration; if the saved-struct has a version, bump it and default the new fields on load so old settings files still load — do NOT break existing settings). Decide + implement persistence for `pin_fail_count` (state which mechanism; must survive power-off).
- [ ] **Step 3:** On correct PIN → reset `pin_fail_count = 0` (persist). On wrong PIN → `++pin_fail_count` (persist); if `wipe_enabled && pin_fail_count >= pin_fails_before_wipe` → `security_wipe_user_data()`, reset counter, show an on-screen notice (reuse an existing popup/notice mechanism), stay locked.
- [ ] **Step 4:** Build m5stick_s3 clean (verify Task 1+2 compile). Fix desktop-only compile errors.
- [ ] **Step 5: Commit.**

---

## Task 3: Lock-on-boot

**Files:**
- Modify: `applications/services/desktop/desktop.c` (init/first-scene path) + `DesktopSettings` (add `bool lock_on_boot;` default true).

- [ ] **Step 1:** Find where desktop chooses its first scene / initial locked state at startup (grep the first `scene_manager_next_scene` / how `desktop->locked` is initialized; the main scene decides locked vs unlocked). 
- [ ] **Step 2:** At init, if `settings.lock_on_boot && desktop_pin_code_is_set()`, start in the locked state (call `desktop_lock` or route to the locked scene as the first scene) so the device comes up locked. If NO pin is set, never lock on boot (must not soft-lock the user out). Confirm the unlock flow from the boot-locked state works (it reuses the normal locked scene).
- [ ] **Step 3:** Build m5stick_s3 clean.
- [ ] **Step 4: Commit.**

---

## Task 4: Settings UI

**Files:**
- Modify: `applications/settings/desktop_settings/scenes/*` (the settings menu that edits DesktopSettings) — add: `Lock on Boot` (on/off), `Wipe on PIN fail` (on/off), `PIN fails before wipe` (Off/3/5/10/15).

- [ ] **Step 1:** Grep the desktop_settings scene that lists auto-lock/pin options (`desktop_settings_scene_*`), see how an existing on/off + value-list item is added (variable_item_list pattern).
- [ ] **Step 2:** Add the three items, wired to the new DesktopSettings fields, matching the existing item pattern exactly; save on change. `PIN fails before wipe` maps Off→`wipe_enabled=false` or `pin_fails_before_wipe=0`; the numeric values set the threshold.
- [ ] **Step 3:** Build m5stick_s3 clean.
- [ ] **Step 4: Commit.**

---

## Self-Review
- Spec coverage (Feature B): lock-on-boot (Task 3) ✓; wipe-after-N with persisted counter (Task 2) ✓; user-data-dir whitelist + protected dirs, host-tested (Task 1) ✓; settings toggles + threshold (Task 4) ✓; default N=10 + default-on (Task 2/4) ✓.
- Safety: the only destructive function is `security_wipe_user_data()` gated behind an exact-match whitelist predicate that a host test proves rejects `/ext`, asset_packs, dolphin, and non-whitelist paths — no traversal, no input-derived paths.
- No placeholders: whitelist/protected lists are concrete; the recursive-remove storage API name is grep-verified by the implementer (Task 1 Step 3); settings-struct version migration explicitly handled (Task 2 Step 2).
- Persistence-across-power-off is a stated hard requirement with an explicit implementer decision point (Global Constraints + Task 2).
- Mirror deferred to combined end.
