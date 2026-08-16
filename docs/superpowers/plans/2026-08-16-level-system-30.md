# 30-Level Dolphin System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Extend the dolphin level system from 3 hardcoded levels to a 30-level XP curve, generalize the four level functions to a threshold table, ensure the passport/level-up UI renders to 30, and award XP from the new tool flows.

**Architecture:** Replace the two `#define` thresholds in `dolphin_state.c` with a single 30-entry `static const uint32_t` threshold table and rewrite `dolphin_get_level` / `dolphin_state_is_levelup` / `dolphin_state_xp_above_last_levelup` / `dolphin_state_xp_to_levelup` to walk it. Verify with a standalone host test (pure integer math). Then check the passport/level-up rendering and add XP hooks.

**Tech Stack:** C (ESP-IDF Flipper port), the dolphin service, WSL gcc for the host test.

**Spec:** `docs/superpowers/specs/2026-08-16-port-ux-security-pack-design.md` (Feature A)

## Global Constraints
- Superset `src_m5stick_s3` only; mirror to the other two trees at the combined end (NOT in this plan).
- These are UX features — NOT `PUBLIC_BUILD`-gated (present in both builds).
- Save-format unchanged: `DolphinStoreData` layout identical; do NOT change `DOLPHIN_STATE_HEADER_MAGIC`/`VERSION` (old saves stay valid).
- Commit discipline: unrelated prior-session changes (nrf24, kia) + the DESFire branch changes exist in the tree; `git add` ONLY this plan's explicit paths, never `git add -A`. Commit trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01G1b3g8F3qEPrP6Esy5nYHq`. Use heredoc/-F (Git Bash backtick gotcha).
- Build via PowerShell + `build_m5stick_s3.bat`; stale-log gotcha (delete build dir, fresh-log verify, 0 errors, fresh binary).

## The Curve (single source of truth)
30 levels → 29 boundary values `b[0..28]`, where `b[i]` = the last `icounter` value of level `(i+1)` (transition to level `(i+2)` happens at `b[i]+1`). Level 1 = `[0, b[0]]`, level 30 = `(b[28], ∞)`.
- Formula: `b[i] = round(300 * (i+1)^1.574)` for `i = 0..28`. This gives `b[0] = 300` (fast first level, preserves stock L1→L2), a smooth front-loaded acceleration, and `b[28] ≈ 60000` (a realistic long-term grind to L30 given the daily-XP caps). Precompute the 29 literals into a `static const uint32_t` array (C has no compile-time `pow`).
- Requirements the host test enforces (so exact arithmetic is self-checking): strictly increasing; `b[0] == 300`; `55000 <= b[28] <= 65000`; array length 29.

---

## Task 1: 30-level threshold table + generalized level functions (host-tested)

**Files:**
- Modify: `applications/services/dolphin/helpers/dolphin_state.c` (replace the 2 `#define`s + 4 functions)
- Modify: `applications/services/dolphin/helpers/dolphin_state.h` (if the level count needs a public `#define DOLPHIN_LEVEL_COUNT 30`)
- Create: `tools/dolphin_level_host_test/Makefile`, `test_level.c` (standalone host test of the 4 functions)

**Interfaces (unchanged signatures — behavior generalized):**
- `uint8_t dolphin_get_level(uint32_t icounter)` → 1..30
- `bool dolphin_state_is_levelup(uint32_t icounter)` → true iff `icounter` equals a boundary
- `uint32_t dolphin_state_xp_above_last_levelup(uint32_t icounter)` → icounter minus the lower bound of the current level
- `uint32_t dolphin_state_xp_to_levelup(uint32_t icounter)` → boundary-of-current-level minus icounter; 0 at level 30 (cap)

- [ ] **Step 1: Write the host test** (`tools/dolphin_level_host_test/`)

The 4 functions are pure integer math over the table. Copy the table + the 4 function bodies into a host-compilable unit (either `#include` the relevant section via a small extraction, or — simplest — define the table + functions in a shared `dolphin_level.inc`/small header that both `dolphin_state.c` and the test include). Prefer: extract the table + 4 functions into `dolphin_level.c/.h` (pure, no furi deps) that `dolphin_state.c` includes/links, so the test compiles them directly. Test asserts:
```c
// table sanity
assert(DOLPHIN_LEVEL_COUNT == 30);
for(int i=1;i<29;i++) assert(thresholds[i] > thresholds[i-1]); // strictly increasing
assert(thresholds[0] == 300);
assert(thresholds[28] >= 55000 && thresholds[28] <= 65000);
// level boundaries
assert(dolphin_get_level(0) == 1);
assert(dolphin_get_level(300) == 1);
assert(dolphin_get_level(301) == 2);
assert(dolphin_get_level(thresholds[28]) == 29);
assert(dolphin_get_level(thresholds[28]+1) == 30);
assert(dolphin_get_level(0xFFFFFFFF) == 30);
// is_levelup fires exactly on boundaries
assert(dolphin_state_is_levelup(300));
assert(!dolphin_state_is_levelup(301));
assert(dolphin_state_is_levelup(thresholds[28]));
// xp helpers
assert(dolphin_state_xp_to_levelup(0) == 300);
assert(dolphin_state_xp_to_levelup(0xFFFFFFFF) == 0); // capped at L30
assert(dolphin_state_xp_above_last_levelup(0) == 0);
```

- [ ] **Step 2: Run host test → RED** (`wsl bash -lc "cd tools/dolphin_level_host_test && make test"`) — undefined symbols / missing table.

- [ ] **Step 3: Implement** — create `dolphin_level.{c,h}` with `#define DOLPHIN_LEVEL_COUNT 30`, the precomputed 29-value `static const uint32_t dolphin_level_thresholds[29]` (compute via the formula), and the 4 functions walking the table. In `dolphin_state.c`, remove the 2 `#define`s + the 4 inline function bodies and delegate to the new unit (keep the same external symbol names so callers are unaffected — `dolphin.c`, passport, etc.). Keep `dolphin_state_increase_level`'s `furi_assert(is_levelup(...))` working (it now asserts against any boundary).

- [ ] **Step 4: Run host test → GREEN** (all asserts pass).

- [ ] **Step 5: Commit** (`dolphin_level.{c,h}`, `dolphin_state.{c,h}`, `tools/dolphin_level_host_test/`).

---

## Task 2: Passport & level-up UI render to 30

**Files:**
- Inspect/Modify: `applications/settings/dolphin_passport/*` (the passport draw code) and the level-up popup (grep `dolphin` in `applications/services/desktop` or wherever the level-up scene lives).

- [ ] **Step 1:** Grep the passport + level-up render code for hardcoded `3`, a fixed level cap, or a fixed-width XP bar that assumes ≤3 levels or the old thresholds. Identify what breaks at level > 3.
- [ ] **Step 2:** Fix the rendering so: the level number displays 1..30; the XP progress bar uses `dolphin_state_xp_above_last_levelup` / `_xp_to_levelup` (already generalized) so it scales per-level; no truncation/overflow at high levels or high XP values. If the passport already uses those helpers generically, no change is needed — confirm and note it.
- [ ] **Step 3:** Build the m5stick_s3 firmware (PowerShell, `build_m5stick_s3.bat`, stale-log gotcha) to confirm the passport/dolphin changes compile clean (this is the compile check for Tasks 1-2). Fix any compile errors (dolphin files only).
- [ ] **Step 4: Commit.**

---

## Task 3: XP hooks from new tool flows

**Files:**
- Modify: `applications/main/nfc/scenes/nfc_scene_mf_desfire_dict_attack.c` (award XP on successful DESFire key recovery) — PRIVATE-gated (already inside `#ifndef PUBLIC_BUILD`).
- Modify: any other new offensive tool entry point that currently awards no XP (grep for how existing apps call the dolphin deed API, e.g. `dolphin_deed(DolphinDeed...)` via the `Dolphin` record).

**Interfaces:**
- Consumes: the existing dolphin-deed API. Grep for the exact call pattern (likely `Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN); dolphin_deed(dolphin, DolphinDeedNfc...); furi_record_close(RECORD_DOLPHIN);` or an app-level `dolphin_deed(...)` helper). Use an EXISTING `DolphinDeed` value that fits NFC usage — do NOT add a new enum value (it ripples into weight/limit tables). State which deed you used.

- [ ] **Step 1:** Grep for the canonical dolphin-deed call used by an existing NFC scene (e.g. on a successful NFC read/save) and its `DolphinDeed` value + record name.
- [ ] **Step 2:** In the DESFire dict-attack scene, on a run that recovers ≥1 key (`found_count > 0`), emit that deed once. Keep it inside the existing `#ifndef PUBLIC_BUILD` gate. Respect the record open/close pattern exactly as the existing scenes do (no leaked record handle).
- [ ] **Step 3:** Build m5stick_s3 clean (compile check). Fix dolphin/nfc-scene compile errors only.
- [ ] **Step 4: Commit.**

---

## Self-Review
- Feature A spec coverage: 30-level table + generalized functions (Task 1) ✓; passport/level-up to 30 (Task 2) ✓; XP hooks (Task 3) ✓; save-format unchanged (Global Constraints + Task 1 delegates, no struct change) ✓; host-test verification (Task 1) ✓; build verification (Tasks 2-3) ✓.
- No placeholders: the curve is a concrete formula with host-test-enforced invariants (exact literals computed by the implementer, self-checked). Deed value is grep-verified against an existing one (Task 3), not invented.
- Type consistency: the 4 function signatures are unchanged; `DOLPHIN_LEVEL_COUNT`/`dolphin_level_thresholds` are the only new symbols, used consistently between `dolphin_level.{c,h}`, `dolphin_state.c`, and the host test.
- Mirror to other trees is explicitly deferred to the combined end (Global Constraints), not this plan.
