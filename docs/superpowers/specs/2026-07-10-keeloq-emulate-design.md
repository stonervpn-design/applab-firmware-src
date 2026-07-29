# KeeLoq "By Manufacturer" — direct Emulate — Design Spec

**Date:** 2026-07-10
**Status:** Approved (design confirmed). Small extension of the KeeLoq by-manufacturer generator.

## Goal

Add a **direct "Emulate"** path from the KeeLoq "By Manufacturer" generator so the
user can immediately act as the configured fob — routing straight into the existing
**Transmitter** scene (which already auto-advances the rolling counter each send and
maps Up/Down/Left/Right/OK to different KeeLoq button commands) — instead of the
save-then-reopen detour.

## Context (what already exists — reuse, don't rebuild)

- The by-manufacturer flow: Add Manually → "KL: By Manufacturer" → manufacturer
  picker (`subghz_scene_keeloq_mfr.c`) → SetSerial → SetButton → SetCounter →
  generate (`subghz_scene_set_counter.c:~292`) → currently routes to
  `SubGhzSceneSaveName`.
- The **Transmitter scene** (`subghz_scene_transmitter.c`) IS a fob emulator:
  counter-advance on each TX (`rolling_counter_mult=1` default) + custom buttons
  (`SUBGHZ_CUSTOM_BTN_UP/DOWN/LEFT/RIGHT/OK`). It reads the generated protocol data
  from `subghz_txrx_get_fff_data`, which generation has already populated — so
  routing there directly works (no save needed).
- The Transmitter has no Save action (Emulate is send-only; Save stays available
  via the "Save" branch).

## Design

1. **Flag the flow:** add `bool subghz->keeloq_mfr_emulate_flow` to the app struct
   (or reuse `gen_info`). Set it `true` in the manufacturer-picker selection
   callback (`subghz_scene_keeloq_mfr.c`) so only the by-manufacturer path is
   affected. Reset it defensively on Add-Manually start.
2. **Branch after generation:** in `subghz_scene_set_counter.c`, at the
   generation-success point that currently does `next_scene(SubGhzSceneSaveName)`,
   if `keeloq_mfr_emulate_flow` is set → go to a new small **action** scene instead.
3. **Action scene** `subghz_scene_keeloq_action.c`: a 2-item submenu —
   - **Emulate** → clear the flag, `next_scene(SubGhzSceneTransmitter)`.
   - **Save** → clear the flag, `next_scene(SubGhzSceneSaveName)` (existing behavior).
   Back → returns to the counter/flow.

Only the by-manufacturer path gets the choice; all other Add-Manually protocols and
the hardcoded KeeLoq entries are unchanged (flag stays false for them).

## Non-Goals

- No change to the Transmitter/emulator itself (counter-advance + custom buttons
  already work).
- No change to other protocols' Add-Manually flow.
- No new emulation engine — this is a routing/UX convenience.

## Components

- Modify: `subghz_i.h` (flag), `subghz_scene_keeloq_mfr.c` (set flag),
  `subghz_scene_set_counter.c` (branch), `subghz_scene_start.c`/set_type reset
  (defensive clear).
- Create: `subghz_scene_keeloq_action.c` + register in `subghz_scene_config.h`.

## Testing

- **Compile gate:** `build_ci.bat`.
- **On-hardware:** Add Manually → KL: By Manufacturer → pick a mfr → serial/button/
  counter → the Emulate/Save choice appears → **Emulate** → Transmitter opens →
  OK sends (counter advances each press), Up/Down/Left/Right send different buttons.
  **Save** → SaveName as before. Hardcoded KeeLoq entries + other protocols still go
  straight to SaveName (no choice shown).

## Open Items (planning)

1. Exact app-struct field placement + where to reset the flag (Add-Manually start /
   set_type on_enter) — confirm during planning.
2. Confirm the Transmitter reads generated (unsaved) data correctly when entered
   from this path (it should — same data SaveName would save).
