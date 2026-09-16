# nRF24 Jammer PA-Level Toggle — Port Report (cardputer_adv -> m5stick_s3)

Date: 2026-08-16
Branch: feat/nrf24-fusion-spectrum (uncommitted working-tree changes, per instructions)

## Method
Diffed each REFERENCE file (`src_cardputer_adv/.../nrf24`) against its TARGET
counterpart (`src_m5stick_s3/.../nrf24`). For files 1-4 the only delta was the
PA-toggle feature, so it was applied verbatim. File 5 (`scene_jam.c`) had an
UNEXPECTED extra diff (see below), so the PA delta was applied surgically by
hand rather than by clobbering the file, preserving the unrelated code.
`scene_spectrum.c` was edited per the minimal-diff instructions only (its
per-channel-acquire sweep loop was not touched).

## Files edited

### 1. `applications/main/nrf24/helpers/nrf24_export.h`
Diff-clean vs REFERENCE (no unexpected differences). Applied:
- `nrf24_settings_load` gained a `uint8_t* pa_level` 3rd param.
- `nrf24_settings_save` gained a `uint8_t pa_level` 3rd param.
- Added the doc comment above `nrf24_settings_load` describing PA_LEVEL
  semantics (0..3 Min/Low/High/Max, optional/defaults to 3).

### 2. `applications/main/nrf24/helpers/nrf24_export.c`
Diff-clean vs REFERENCE. Applied:
- `nrf24_settings_load`: added `uint8_t* pa_level` out-param; reads optional
  "PaLevel" key via `flipper_format_read_uint32`, defaulting to 3 (Max) on a
  missing key or any value > 3; does not affect the function's `ok` return.
- `nrf24_settings_save`: added `uint8_t pa_level` param; clamps to <=3 and
  writes it as the "PaLevel" key.

### 3. `applications/main/nrf24/views/nrf24_jam_view.h`
Diff-clean vs REFERENCE. Applied:
- `Nrf24JamModel` gained `uint8_t pa_level;`.
- `Nrf24JamEvent` enum gained `Nrf24JamEventPaDown` / `Nrf24JamEventPaUp`.
- Added decl `const char* nrf24_jam_pa_name(uint8_t pa_level);`.

### 4. `applications/main/nrf24/views/nrf24_jam_view.c`
Diff-clean vs REFERENCE. Applied:
- Added `nrf24_jam_pa_name()` (0..3 -> "Min"/"Low"/"High"/"Max").
- Draw callback now renders "PA:<name>" top-right via
  `canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, pa)`.
- Bottom hint line changed to include "L/R:PA" in both running/idle states
  (`"*JAM OK:stop  L/R:PA"` / `"Dn:mode OK:jam L/R:PA"`).
- Input callback: `InputKeyLeft` -> `Nrf24JamEventPaDown`,
  `InputKeyRight` -> `Nrf24JamEventPaUp`.

### 5. `applications/main/nrf24/scenes/scene_jam.c`
**UNEXPECTED DIFF FOUND** — beyond the PA-toggle delta, the TARGET file
already contained a block REFERENCE does not have: a "TEMP DIAGNOSTIC
(weak-RF debug)" `FURI_LOG_I(TAG, "JAMDIAG ...")` call (dumping Pvar,
RF_SETUP, CONFIG, ch0) inside the jammer/flood-start branch of the worker
loop, marked "remove after root cause". Per task instructions this was NOT
clobbered — the file was NOT replaced wholesale from REFERENCE; instead the
PA-toggle delta was applied by hand around the existing diagnostic code,
which is fully preserved (including its inline comment and log format
string, unchanged).

Applied (PA delta only):
- `JamCtx` gained `volatile uint8_t pa_level;`.
- Worker: added `uint8_t cur_pa = 0xFF;` local (forces first (re)start to
  apply the loaded PA level); (re)start condition became
  `if(want && (!active || mi != cur || ctx->pa_level != cur_pa))`.
- Jammer/flood start: restored `uint8_t pa = ctx->pa_level;` and passed `pa`
  (was hardcoded `PA_MAX`) to `nrf24_hw_flood_start_ex` /
  `nrf24_hw_jammer_start_ex`. The diagnostic `FURI_LOG_I` block immediately
  after was left untouched (it does not reference PA_MAX/pa, so no further
  edit was needed there).
- After a (re)start completes: added `cur_pa = ctx->pa_level;` alongside the
  existing `cur = mi; cur_flood = flood; cur_reactive = reactive;` lines.
- `nrf24_app_scene_jam_on_enter`: `saved_mode`/`saved_pa` locals (default
  `PA_MAX`), calls the new 3-arg `nrf24_settings_load(&saved_mode,
  &saved_dwell, &saved_pa)`, sets `ctx->pa_level = saved_pa;` and
  `model->pa_level = saved_pa;` in the view-model init block.
- `nrf24_app_scene_jam_on_event`: added `Nrf24JamEventPaDown` (decrement,
  floor 0) and `Nrf24JamEventPaUp` (increment, cap 3) cases, each updating
  `g_ctx->pa_level` and the view model's `m->pa_level`.
- `nrf24_app_scene_jam_on_exit`: `nrf24_settings_save((uint8_t)g_ctx->mode,
  g_ctx->saved_dwell, g_ctx->pa_level);` (was 2-arg).

### `applications/main/nrf24/scenes/scene_spectrum.c` (minimal edit, not a full port)
Also calls `nrf24_settings_load/save`, so it required updating to the new
3-arg signatures — done via the MINIMAL edits specified in the task, with
the per-channel-SPI-acquire sweep loop left completely untouched:
- Added `uint8_t saved_pa;` field to `Nrf24SpectrumCtx`.
- `nrf24_settings_load(&saved_mode, &saved_dwell)` ->
  `nrf24_settings_load(&saved_mode, &saved_dwell, &saved_pa)` with a new
  local `uint8_t saved_mode = 0, saved_pa = 3;`; stores `ctx->saved_pa =
  saved_pa;`.
- `nrf24_settings_save(g_ctx->saved_mode, g_ctx->dwell_us)` ->
  `nrf24_settings_save(g_ctx->saved_mode, g_ctx->dwell_us, g_ctx->saved_pa)`.

**Confirmation: scene_spectrum.c's per-channel-acquire sweep loop
(`for(uint8_t ch = 0; ch < NRF24_SPECTRUM_CHANNELS && !ctx->stop; ch++) {
nrf24_hw_acquire(); ... nrf24_hw_release(); ... }`) is byte-for-byte
unchanged** — verified by re-reading the file after edits; no lines in that
loop or its surrounding comment were touched.

## Other call sites checked
Grepped the whole `src_m5stick_s3` tree for `nrf24_settings_load`,
`nrf24_settings_save`, `nrf24_jam_pa_name`, and `pa_level`. Only the 6
production `.c`/`.h` files above reference the changed signatures/symbols;
all are updated consistently. `nrf24_hw.c`'s existing
`nrf24_hw_jammer_start_ex`/`nrf24_hw_flood_start_ex`/
`nrf24_negotiate_rf_setup` functions already took a `pa_level` parameter
independently of this port (pre-existing infra) and needed no changes.
Historical planning docs under `docs/superpowers/` still show the old 2-arg
signature in prose/code-block examples — these are not compiled sources and
were left alone.

`src_cardputer_adv` was not touched.

## Build result
Command: `build_lilygo_t_embed.bat` via `Start-Process cmd.exe ... -Wait`.

- Last SENTINEL token in log: **SENTINEL_OK**
- `error:` line count in log: **0**
- Output bin: `Flipper-lilygo_t_embed_cc1101-merged.bin`
  - Size: 3,263,072 bytes
  - Freshly generated at build completion (same-minute mtime as the log's
    final SENTINEL_OK line)
- Log SENTINEL sequence: SENTINEL_START -> SENTINEL_SETTARGET ->
  SENTINEL_BUILD -> SENTINEL_MERGE -> SENTINEL_OK

## Status
DONE. No genuine blockers. The single unexpected diff (TEMP DIAGNOSTIC
logging in `scene_jam.c`) was preserved rather than clobbered, per
instructions, and does not affect the PA-toggle feature's correctness.
Not flashed (per constraints). Not committed (left staged in the working
tree on `feat/nrf24-fusion-spectrum` for the controller).
