# Port UX & Security Pack — Design

**Date:** 2026-08-16
**Status:** Approved (design okayed in chat)
**Scope:** Three independent subsystems on the M5Stick-S3 Flipper port, built on services the fork already ships. Superset `src_m5stick_s3` first, then mirror to `src_cardputer_adv` and `src_lilygo_t_embed_cc1101` (no-strip parity). Flashes together with the DESFire attack branch.
**Decisions (from user):** 30 levels + new XP hooks; PIN wipe = **wipe SD user-data dirs** after N (default 10, configurable) + lock-on-boot; **full swappable Asset Packs** (phased).

Each subsystem gets its own implementation plan and its own build/review cycle. Build order: A (level) → B (security) → C (asset packs).

---

## Feature A — 30-Level System + XP Hooks

**Current state (verified):** `applications/services/dolphin/helpers/dolphin_state.c` caps at **3 levels** via `LEVEL2_THRESHOLD 300` / `LEVEL3_THRESHOLD 1800`, hardcoded into four functions: `dolphin_get_level`, `dolphin_state_is_levelup`, `dolphin_state_xp_above_last_levelup`, `dolphin_state_xp_to_levelup`. The deed/XP engine (`dolphin_state_on_deed`), daily limits, and `dolphin_state_increase_level` (asserts `is_levelup`, then `++icounter`) already work and are level-count-agnostic *except* through those four functions.

**Design:**
- Introduce a single source of truth: `static const uint32_t dolphin_level_thresholds[DOLPHIN_LEVEL_COUNT]` where `DOLPHIN_LEVEL_COUNT = 30`. Entry `i` = the `icounter` value at which the dolphin transitions **into** level `i+1` (i.e., the upper bound of level `i`). Index 0 = threshold to leave L1. Monotonically strictly increasing.
- **Curve:** front-loaded (early levels fast, later grind). Preserve the stock feel at the low end (`L1→L2 = 300`, `L2→L3 = 1800`) for continuity, then continue with an accelerating curve to L30. Exact table lives in the plan; it must be strictly increasing and fit `uint32_t`.
- Rewrite the four functions to **walk the table** instead of the two `if/else` branches:
  - `dolphin_get_level(icounter)` → highest level whose lower bound `icounter` has reached (clamped to 30).
  - `dolphin_state_is_levelup(icounter)` → `icounter` equals any table entry (drives the level-up ceremony).
  - `dolphin_state_xp_above_last_levelup` / `_xp_to_levelup` → relative to the surrounding table entries; at max level, `xp_to_levelup` returns 0 (capped).
- **Cap behavior:** at L30, `on_deed`'s `xp_to_levelup==0` path already stops icounter growth — preserve that (no overflow, no infinite level).
- **Save-format compatibility:** `DolphinStoreData` (icounter, butthurt, limits) is unchanged — only the interpretation of icounter changes. Bump `DOLPHIN_STATE_HEADER_VERSION`? NO — the struct layout is identical; existing saves remain valid (an old save's icounter just maps onto the new curve). Do not change the magic/version.
- **XP hooks (new):** emit `dolphin_deed(...)` from tool flows that currently award no XP — specifically the new DESFire attack scene (on a successful key recovery) and any other new offensive tools. Use existing `DolphinDeed` values where a fitting one exists (e.g. an NFC deed); do **not** invent new enum values unless a fitting one is absent (a new deed value ripples into weight/limit tables). Respect the daily-limit engine (already handled by `on_deed`).
- **Passport / level-up UI:** `dolphin_passport` app and the level-up popup must render levels up to 30 (verify no hardcoded "3" / no fixed-width bar that breaks past L3). Adjust drawing if needed.

**Verification:** the four level functions are pure integer math → a small standalone host test (gcc, like the DESFire harness) asserts: level boundaries (icounter at/just-below/just-above each threshold → correct level), `is_levelup` fires exactly on table entries, monotonicity of the table, and cap behavior at L30. Plus on-device build.

**Non-goals:** no perks/feature-unlocks by level (cosmetic titles only if trivial); no change to the butthurt/mood system.

---

## Feature B — Security: Lock-on-Boot + Wipe-After-N

**Current state (verified):** `applications/services/desktop/desktop.c` has a full auto-lock timer system (`desktop_auto_lock_arm/inhibit/start_auto_lock_timer`, `settings.auto_lock_delay_ms`, `usb_inhibit_auto_lock`), a `desktop_locked` state, and PIN support via `desktop_settings` + the lock/PIN-input scenes. `DesktopSettings` is a saved struct.

**Design:**
- **Lock on boot:** in desktop init, if a PIN code is configured, enter the locked state immediately (show the lock screen at startup) instead of the unlocked desktop. Gate behind a `DesktopSettings.lock_on_boot` bool (default **on** when a PIN exists). Must not lock if no PIN is set (avoids soft-locking the user out).
- **Wipe after N failed PINs:**
  - Add `DesktopSettings.pin_fails_before_wipe` (default **10**, 0 = disabled) and `DesktopSettings.wipe_enabled` (default on), plus a persisted counter `pin_fail_count`.
  - **Persistence across reboots is mandatory** (the whole anti-theft point): store `pin_fail_count` in a saved struct that survives power-cycle — extend `DesktopSettings` (already saved to internal storage) or a dedicated tiny saved struct. Increment on each wrong PIN in the lock/PIN-input scene; reset to 0 on a correct unlock.
  - On reaching the threshold: **wipe the user-data directories on SD**, then reset the counter. Wipe set (preserve everything else): `/ext/nfc`, `/ext/subghz`, `/ext/lfrfid`, `/ext/infrared`, `/ext/badusb`, `/ext/rfid`, `/ext/ibutton`, `/ext/apps_data` user dumps — i.e. user captures/dumps. **Preserve** firmware/system, `/ext/asset_packs`, `/ext/dolphin`, and settings. The exact dir list lives in the plan and must be conservative (recursive-delete only whitelisted capture dirs; never `/ext` root).
  - After wipe, show a clear on-screen notice and stay locked.
- **Safety guards:** a single well-named helper `desktop_security_wipe_user_data()` with the whitelist as a `static const char* const[]`; recursive delete via the storage API; log each dir. No path derived from untrusted input. A settings toggle lets the user disable the wipe entirely.
- **Settings UI:** add entries in `desktop_settings` for `lock_on_boot`, `wipe_enabled`, and `pin_fails_before_wipe` (a value picker: Off/3/5/10/15).

**Verification:** the wipe target-list logic (which dirs qualify) is pure and host-testable (assert the whitelist, assert `/ext` root and asset_packs are never in it). Reboot-persistence and the actual wipe are on-device HW tests (user), with a big warning. Lock-on-boot verified on device. Build-verified.

**Non-goals:** no full-SD format; no remote wipe; no self-destruct/brick.

---

## Feature C — Full Swappable Asset Packs (phased)

**Current state (verified):** `applications/services/desktop/animations/animation_manager.c` + `animation_storage.c` load dolphin idle/boot animations from an external manifest (stock Flipper external-animation system, `/ext/dolphin`). Icons are compiled-in (`assets`), resolved via the `Icon`/`icon_animation` API.

**Design (Momentum-style asset packs):**
- **Pack layout:** `/ext/asset_packs/<PackName>/` containing `Anims/` (animations + `manifest.txt`), `Icons/` (optional icon overrides mirroring the built-in icon tree), `Fonts/` (optional). Selected pack name stored in settings (`DesktopSettings.asset_pack` or a dedicated setting).
- **Phase 1 (ship first — the visible majority):**
  - **Pack selection UI:** a selector (in `desktop_settings` or a small "Asset Packs" settings app) that enumerates `/ext/asset_packs/*` and stores the choice.
  - **Animations from pack:** point `animation_storage`/`animation_manager` at the selected pack's `Anims/` + manifest (fall back to built-in/`/ext/dolphin` when "Default" or a file is missing).
  - **Custom boot logo:** load the pack's boot splash if present, else the built-in.
- **Phase 2 (after Phase 1 works on HW):**
  - **Icon/font override:** hook icon resolution so the active pack's `Icons/` (and `Fonts/`) override built-ins per-asset, with graceful fallback. This is the heavy Momentum-style part (icon lookup indirection); isolate it so a missing/partial pack can never brick the UI.
- **Robustness:** a malformed/partial pack must degrade to built-in assets, never crash or blank the screen. All pack I/O is fail-safe with fallback.
- **Ship an example pack** under the staged `/ext` so the feature is demonstrable.

**Verification:** manifest-parse + pack-enumeration logic host-testable where pure; animation load + boot logo + icon override are on-device (user). Build-verified. Phase 1 must be HW-confirmed before Phase 2.

**Non-goals (this spec):** no pack download/marketplace; no per-app theme scoping; Phase 2 icon override may be deferred to its own plan if Phase 1 reveals scope creep.

---

## Cross-cutting

- **Gating:** these are UX/security features, not offensive tooling — they are **not** `PUBLIC_BUILD`-gated (they belong in both public and private builds). (Contrast with the DESFire attack surface, which is private-only.)
- **Parity:** implement in `src_m5stick_s3`, then mirror to `src_cardputer_adv` + `src_lilygo_t_embed_cc1101`. Input differs per board (2-button on M5Stick per the port's input scheme) — any new UI must be operable with each board's actual button scheme; verify the selectors/pickers are reachable.
- **Build:** PowerShell + `build_m5stick_s3.bat`, stale-log gotcha (delete build dir, fresh-log verify, 0 errors, fresh binary).
- **Combined flash:** after A+B+C land and the DESFire branch is merged in, one combined build → flash M5Stick (COM10) → user HW-tests the lot → mirror + build the other two boards.

## Build order & decomposition
1. **Plan A — Level system** (smallest, self-contained; dolphin_state + passport).
2. **Plan B — Security** (desktop lock-on-boot + wipe-after-N + settings).
3. **Plan C — Asset packs Phase 1** (selection + animations + boot logo); Phase 2 (icon/font override) as a follow-on plan.
