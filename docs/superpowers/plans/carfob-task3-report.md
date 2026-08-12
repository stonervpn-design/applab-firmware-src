# Car-fob emulate-all-protocols — Task 3 report

**Task:** Route every SUPPORTED car protocol (Task 1's 21) into the multi-button rolling
fob view and synthesize+TX each button with counter advance+persist, reusing the existing
txrx TX path and the VAG/KeeLoq counter-jump/persist mechanism. VAG + KeeLoq unchanged.
**Branch:** feat/keeloq-no-desync-clone
**Status:** DONE. Private cardputer build GREEN. Host tests PASS. Committed (no push).

## Commit
Single commit on feat/keeloq-no-desync-clone, parent `430ec1c` (Task 2). It is the branch
HEAD immediately after this task (see `git log -1`). Not pushed.

## Files changed
- `applications/main/subghz/helpers/subghz_fob_buttons.{c,h}` — added
  `subghz_fob_button_field(protocol_name)` (per-protocol write-field name).
- `applications/main/subghz/helpers/subghz_fob_nav.c` — nav for n>4 (2-column grid).
- `applications/main/subghz/helpers/subghz_custom_event.h` — new
  `SubGhzCustomEventSceneReceiverInfoEmulate`.
- `applications/main/subghz/subghz_i.h` — new SubGhz fields (`keeloq_fob_generic`,
  `keeloq_fob_field[16]`, `keeloq_fob_title[24]`) + `subghz_fob_can_emulate` decl.
- `applications/main/subghz/subghz_i.c` — `subghz_fob_can_emulate()` (guarded routing test).
- `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c` — generic on_enter branch,
  generic press path, generic counter advance+persist.
- `applications/main/subghz/scenes/subghz_scene_garage.c` — route saved supported fobs
  into the generic grid.
- `applications/main/subghz/scenes/subghz_scene_receiver_info.c` — "Emul" button + handler
  for captured supported fobs.
- `tests/test_subghz_fob_buttons.c`, `tests/test_fob_nav.c` — host assertions for the new
  field-name provider and n>4 nav.

## Routing entry points (how a supported fob REACHES the scene)
Mirrors how VAG/KeeLoq get in. Two entries, both set `keeloq_fob_generic=true` +
`keeloq_fob_from_garage=true` before `scene_manager_next_scene(..., SubGhzSceneKeeloqFob)`:

1. **Saved (My Garage)** — `subghz_scene_garage.c` on_event. `subghz_key_load` already loads
   the `.sub` into `txrx` fff_data. After the existing CloneMode/VAG check, a new branch:
   `else if(subghz_fob_can_emulate(fff, proto))` → generic grid; everything else still →
   `SubGhzSceneTransmitter`. This is the exact mechanism VAG uses (Save → Garage → fob).
2. **Captured (receiver info)** — `subghz_scene_receiver_info.c`. For a supported car fob the
   free **Left** widget slot shows **"Emul"** (its own callback → `...ReceiverInfoEmulate`)
   instead of the "ID" hub (Center=Send, Right=Save unchanged). The handler stops RX,
   `update_parser` (sets the txrx preset + decoder), copies the captured `.sub`-format frame
   (`subghz_history_get_raw_data`) into txrx fff_data via `stream_copy_full`, then hands off.
   Whole block is `#ifndef PUBLIC_BUILD` (car-side).

The scene's `on_enter` takes the generic branch first when `keeloq_fob_generic`, reads
Protocol from fff, looks up `subghz_fob_buttons(proto,&n)`, `set_title(proto)` +
`set_table(buttons,n,captured_btn,cnt_start,jump)`; VAG/KeeLoq branches are untouched below it.

## Per-protocol button FIELD NAME (DO #1)
`const char* subghz_fob_button_field(const char* protocol_name)` — returns the FlipperFormat
field each encoder reads: `"Button"` for HondaV5/V7/V8/V9, `"BtnLo"` for HondaV2&V3, `"Btn"`
default (also for NULL/unknown — never NULL). Used both to read the captured button (mark the
real tile) and to write the selected button on each press
(`flipper_format_insert_or_update_uint32(fff, field, &code, 1)`). Stored in
`subghz->keeloq_fob_field` at on_enter for the press path. Host-test asserts Kia/Ford/VAG→"Btn",
HondaV5/7/8/9→"Button", HondaV2&V3→"BtnLo", unknown/NULL→"Btn".

## Nav / layout for n>4 (DO #2)
`subghz_fob_nav_move_n` now drives a general **2-column grid** of `ceil(n/2)` rows for `n>=4`.
For **n==4 it is byte-identical to the legacy 2x2** (verified by the existing test loop asserting
`_move_n(i,k,4) == _move(i,k)` for all i,k); n=5..8 add rows, clamping horizontal/vertical moves
that would land past a short last row back onto a valid tile (result always `< n`). VAG (n=3)
still uses the linear branch, unchanged. `fob_tile_rect`'s existing n>4 layout (2 cols, 60x11,
`y=12+(i>>1)*13`) already lays out 5–8 tiles and matches the nav row/col mapping, so tiles are
now both drawn AND reachable. Cap is `SUBGHZ_KEELOQ_FOB_MAX_TILES` (8): `fob_apply_table` clamps
`n`, and the scene logs `FURI_LOG_W` if a provider table exceeds 8 (none do today; max is 6).

## Synthesize + TX per press (DO #4)
On tile OK the scene writes the selected code into the protocol's field (generic) or `"Btn"`
(VAG/KeeLoq), then TX via the SAME path as before — `subghz_tx_start(subghz, fff)` → the
protocol's own encoder rebuilds the frame. No KeeLoq BFT generator for non-keeloq. Cnt is
pre-seeded at on_enter as `cnt_start = cnt_file + jump` (jump = `keeloq_clone_jump`, default 0),
so the first press is a future frame exactly like VAG/KeeLoq.

## Counter advance + persist (DO #5)
VAG/KeeLoq encoders self-advance AND write `Cnt` back into fff_data during `subghz_tx_start`, so
their post-TX path stays `..._refresh_cnt` (read-back, no double-advance). The generic car
encoders do NOT touch `Cnt` (grep confirmed: only ford_v2/fiat_v2v3v5/honda_v4/keeloq persist
Cnt, none of which are grid protocols except keeloq), so the scene owns the roll:
`..._advance_cnt_generic` reads Cnt, +1, `flipper_format_insert_or_update_uint32(fff,"Cnt",...)`,
then `set_sent` to refresh the on-screen counter. Persist is into fff_data only (in-memory), same
as VAG/KeeLoq emulate — the on-disk `.sub` is unchanged until a re-Save. Advance runs ONLY on the
TX-success tick (a failed `subghz_tx_start` restarts RX and never reaches it).

## Guards (DO #6) — `subghz_fob_can_emulate(fff, proto)` in subghz_i.c
- **Honda V2&V3:** the V3 sub-variant bakes the button into the transmitted frame (replay). The
  saved `.sub` carries a `Variant` field; if `Variant==3` the guard returns false → the capture
  falls back to the plain Transmitter (garage) / no "Emul" button (receiver), not the grid. V2
  captures use the grid.
- **Fiat V1:** its encoder deserialize aborts without a `"Hitag2 Key"` field; the guard requires
  that hex field present, else falls back to replay.
- Both entry points call the guard, so an unsupported sub-variant never reaches the grid.
- Missing `Serial`/`Cnt`/button field in the generic on_enter are read with defaults (0), no
  crash; an empty/again-unsupported provider table bails to the previous scene.

## VAG/KeeLoq no-regression
The generic path is a separate branch gated on `keeloq_fob_generic` (only ever set by the two new
routing sites). VAG (`SubGhzFobKindVag`) and KeeLoq (`SubGhzFobKindKeeloq`) `on_enter` branches,
their press path (`"Btn"` write), and their `refresh_cnt` are byte-for-byte unchanged; the flag is
cleared on scene exit. n=4 nav is byte-identical; n<4 (VAG) nav unchanged.

## Build result
`build_adv_only_private.bat` (ESP-IDF v5.4.1 export → `idf.py -B build_cardputer_adv build` →
merge): **Project build complete**, SENTINEL_MERGE, SENTINEL_OK, `Flipper-cardputer_adv-merged.bin`
written (0x300160 bytes). No `error:`/FAILED. My new functions emit zero warnings (pre-existing
warnings like `subghz_scene_receiver_info_is_psa` unused predate this task). Host tests:
`test_subghz_fob_buttons OK`, `test_fob_nav OK` (clean `-Wall -Wextra`).

## Concerns / notes for Task 4
- **Counter is cosmetic for the "replay-rolling-field" subset.** Task 1 flagged SantaFe / Ford V3
  / Fiat V1 as rebuilding the frame from the button but replaying the stored rolling field, so
  advancing `Cnt` there does not change the on-air frame (button DOES change). This matches the
  plan (counter-advance is the persist step); Task 4's round-trip sweep should confirm per-protocol
  whether each grid encoder actually re-derives from `Cnt` vs replays.
- **receiver "Emul" replaces the "ID" hub on the Left slot** for supported protocols (only 3
  widget slots exist; Center=Send/Right=Save are kept). Non-supported dynamic captures still get
  "ID". If keeping both is desired, a follow-up could route Emulate from inside the ID/Analyze hub
  instead.
- **Captured path preset** comes from `update_parser` (txrx preset), TX reads txrx preset not the
  fff — verified against `subghz_txrx_tx_start`. The stream copy carries Frequency/Preset too but
  they are informational on this path.
- Not HW/RF-tested (Task 4 host sweep + on-device verification pending).
