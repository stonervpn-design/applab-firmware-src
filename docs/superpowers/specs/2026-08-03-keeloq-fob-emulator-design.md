# KeeLoq Fob Emulator + Auto-Clone — Design Spec

**Date:** 2026-08-03
**Status:** Design (pending user review)
**Boards:** `src_m5stick_s3` + `src_cardputer_adv` (private builds only)
**Branch:** continues `feat/keeloq-no-desync-clone` (extends the no-desync clone engine)
**Builds on:** [[keeloq-no-desync-clone]] — the clone `.sub` (`kdev`+serial+counter), the forward-pair TX, and the `can_clone` gate already exist.

## Goal

Two connected UX upgrades on top of the no-desync clone:

1. **Auto-clone on capture** — a KeeLoq capture that is "decoded enough" (known manufacturer key + recovered counter, i.e. the existing `can_clone` gate) becomes **one tap to emulate**: the Clone action builds the clone with a default jump (100) and opens the Fob Emulator directly, with no SetJump prompt and no generic transmitter.
2. **On-screen fob emulator** — a custom view that looks like a car remote: a 2×2 grid of labelled + iconed buttons (Lock / Unlock / Trunk / Panic). You move the highlight and press to send. Because the clone holds `kdev`, **every button sends a cryptographically valid frame** (not just the captured one), each via the no-desync forward pair.

## Key facts this design relies on (verified in code)

- The clone transmits via `subghz_protocol_encoder_keeloq_get_upload`'s `clone_active` branch (`keeloq.c`), which emits the `[N, N+1]` pair using `instance->clone.btn` and advances/persists the counter in `fff_data` (`Cnt`/`CloneSynced`).
- The generated clone lives in the txrx **in-memory `fff_data`** (`subghz_txrx_get_fff_data`); `subghz_tx_start(fff_data)` re-deserializes it into a fresh encoder per press. So **changing the `Btn` field in `fff_data` between presses changes which button is sent, while `Cnt`/`CloneSynced` persist** — the counter keeps advancing across button changes. This is the whole mechanism the fob view needs; no new counter plumbing.
- KeeLoq button nibbles: `0x1 0x2 0x4 0x8` (+ combos). The plaintext hop encodes the button, so with `kdev` we can mint a valid frame for **any** nibble. The captured fob only proves *one* real button; the others are synthesized (and are no-ops on a target that lacks them).
- Input reality: **Cardputer** has real arrow keys; **M5Stick S3** has a 2-button scheme (no native arrows — see [[m5sticks3-input-and-open-bugs]]). Per the user: arrows on the Cardputer, tap-to-cycle on the M5Stick.

## Confirmed decisions

1. **Button labels:** car-fob labels **+ icons** — 🔒 Lock (`0x1`), 🔓 Unlock (`0x2`), 🚗 Trunk (`0x4`), ⚠ Panic (`0x8`). The tile matching the fob's captured button is marked as the confirmed-real one.
2. **Auto-clone:** open a clonable capture → auto-build clone (jump 100) → straight into the Fob Emulator. Nothing written to SD unless you Save.
3. **Input:** Cardputer = arrow keys move the highlight, Enter/OK sends. M5Stick = tap cycles the highlight, OK sends, KEY2 = back.

## Design

### A. Behavior: Clone action → Fob Emulator (skip the jump prompt)

Both existing Clone entry points change their *destination*:
- Rolling-Code Analyzer "Clone" (center button) and the By-Manufacturer "Clone (no desync)" action currently route → SetJump → generic Transmitter.
- New: they build the clone with a **default jump** (100, from `subghz->keeloq_clone_jump` seeded to 100) and route → **`SubGhzSceneKeeloqFob`** directly.
- SetJump is **retained** but demoted to an *optional* adjust-jump step reachable from inside the Fob Emulator (not on the hot path). Its existing routing (→ Transmitter) stays for anyone who wants the plain transmitter.

"Decoded enough" is exactly the current `can_clone` predicate (KeeLoq + known mfr + recovered `Cnt`, excludes Unknown/BFT/Erreka/AN-Motors). No change to the gate.

### B. The Fob Emulator (new view + scene)

**View** `subghz_keeloq_fob_view.{c,h}` — a self-contained `View` (mirrors the existing `subghz` transmitter view pattern):
- Draws a 2×2 grid of tiles. Each tile = a small canvas-drawn glyph + label:
  - 🔒 `LOCK` / 🔓 `UNLOCK` / 🚗 `TRUNK` / ⚠ `PANIC`.
- Glyphs are **drawn with canvas primitives** (box/line/arc), NOT compiled image assets — dodges the port's no-Pillow icon-build snag ([[applab-custom-build-service]] CI Pillow gotcha). Labels are the primary identifier; glyphs are accents.
- **Selected tile inverted**; the tile whose nibble == the captured button shows a small "•" (real) marker.
- Bottom status line: `cnt 0x<hhhh> · jump <n> · <state>` where state ∈ {ready, SENT, …}.
- Model (view-owned): `{ uint8_t sel_index; uint8_t codes[4]; uint8_t real_btn; uint16_t cnt; uint16_t jump; bool sending; }`.
- **Input handler** (per-board):
  - Cardputer: `InputKeyLeft/Right/Up/Down` move `sel_index` around the 2×2; `InputKeyOk` (Enter) → fire the **send** callback for `codes[sel_index]`.
  - M5Stick: a short front-button press cycles `sel_index` (`+1 mod 4`); a distinct gesture (long-press / OK event) → send; `InputKeyBack` (KEY2) → exit. Exact gesture mapping confirmed on-device during implementation (the board input layer distinguishes short/long; see [[m5sticks3-input-and-open-bugs]]).
- Callbacks: `on_button(context, uint8_t nibble)` (send), `on_back(context)`.

**Scene** `subghz_scene_keeloq_fob.c` — owns the view, drives TX:
- `on_enter`: reads the just-built clone from `fff_data` (serial/mfr/`Cnt`/captured `Btn`), sets the view model (codes = {0x1,0x2,0x4,0x8}, `real_btn` = captured, `cnt`, `jump`), selects the real button by default, switches to the fob view.
- `on send(nibble)`: `flipper_format_update_uint32(fff_data, "Btn", &nibble, 1)` → `subghz->state_notifications = Tx` → `subghz_block_generic_global.endless_tx = false` (single burst) → `subghz_tx_start(subghz, fff_data)`. The clone forward-pair sends `[target, target+1]` for that nibble and writes back `Cnt`/`CloneSynced`. On completion, re-read `Cnt` into the model and flash "SENT".
- `on_back`: stop TX, (optional) Save prompt (see D), return.
- Stops/guards RX exactly like the Transmitter/receiver-info Save path (hopper off, `subghz_txrx_stop`).

### C. What a press sends

Pressing a tile sends **that button's valid frame** through the no-desync path: `target = (first press) max(last,seen)+jump, else +2`, pair `[target, target+1]`, counter persisted. Lock/Unlock/Trunk/Panic each advance the **same** counter forward, so mixing buttons never desyncs. Buttons the target vehicle lacks are simply ignored by its receiver.

### D. Save fob (optional, lower priority)

Since "nothing saved unless you Save": on **Back**, if the clone isn't already a saved file, offer `Save fob / Discard` (a small confirm). Save writes the current `fff_data` (already a `CloneMode` `.sub` with live `Cnt`) to `/ext/subghz/` via the existing SaveName flow, so it re-opens straight into the fob with the counter resumed. This task is **last** and may ship in a follow-up if the core lands first.

## Components & boundaries

**Create (both trees):**
- `applications/main/subghz/views/subghz_keeloq_fob.{c,h}` — the fob View (render + input + model). One responsibility: present buttons, report selection/send. Host-testable pieces (grid navigation math: `sel_index` transitions for L/R/U/D and cycle) split into a pure helper if practical.
- `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c` — the scene (owns view, drives clone TX). Registered in `subghz_scene_config.h`.
- A `SubGhzViewIdKeeloqFob` view id + view alloc/free in `subghz.c` (mirror an existing custom view's lifecycle).

**Modify (both trees):**
- `subghz_scene_keeloq_action.c` — "Clone (no desync)" → build clone (default jump) → `SubGhzSceneKeeloqFob` (was SetJump).
- `subghz_scene_rolling_analyzer.c` — "Clone" → build clone (default jump) → `SubGhzSceneKeeloqFob`.
- `subghz_scene_set_jump.c` — unchanged behavior, but no longer the hot path (reachable as an optional jump adjust from the fob scene).
- `subghz_custom_event.h` — a `…FobSend`/`…FobBack` event pair if the view routes via custom events (or use direct callbacks).
- Guard all new scene/view code + the Clone entry points with `#ifndef PUBLIC_BUILD` (closes part of the private-only gap in [[fzcard-private-only-guardrail]]).

**Reuse unchanged:** the clone engine (`keeloq.c` get_upload/gen_data/deserialize), `subghz_tx_start`, `fff_data` accessor, `flipper_format_update_uint32`.

## Data flow

capture decodes (can_clone) → open → Clone action builds clone into `fff_data` (jump 100) → `SubGhzSceneKeeloqFob` → view shows 4 tiles, real button marked → user selects tile, presses send → scene writes `Btn` nibble to `fff_data` → `subghz_tx_start` → clone forward-pair TX `[N,N+1]` → `Cnt`/`CloneSynced` persisted → view shows new `cnt` + "SENT" → repeat.

## Error handling / edge cases

- **Non-clonable capture:** the Clone action isn't offered (existing gate), so the fob scene is only ever entered with a valid clone in `fff_data`. `on_enter` still guards: if `fff_data` has no `CloneMode`, show error and return.
- **Counter wrap / synced:** handled entirely by the existing clone core; the fob view just displays `cnt`.
- **Button the fob doesn't have:** valid frame still sent; target no-ops. Not an error. (The "•" marker tells the user which is the confirmed-real one.)
- **M5Stick input ambiguity:** the front button's tap/hold gestures must map cleanly to cycle-vs-send without the dialog-button trap ([[m5sticks3-input-and-open-bugs]]); verified on-device.
- **TX while hopper active:** stop RX/hopper before `tx_start`, restart on Back (mirror the receiver-info Save/TX handlers).
- **Rapid presses:** ignore send while a TX burst is in flight (`sending` flag), re-enable on completion.

## Testing

- **Host unit test:** the pure grid-navigation helper (2×2 selection transitions for L/R/U/D and cycle-wrap) via WSL gcc, mirroring `test_keeloq_clone_core.c`.
- **Compile gate:** `build_ci.bat` per tree; confirm the public build compiles with the fob code guarded out.
- **On-hardware (both boards):**
  1. Capture a clonable KeeLoq fob → open → Clone → **fob emulator opens directly** (no jump prompt).
  2. Cardputer: arrows move highlight, Enter sends each of Lock/Unlock/Trunk/Panic → target reacts to the buttons it has; counter increments on the status line each press.
  3. M5Stick: tap cycles tiles, OK sends, KEY2 backs out.
  4. No-desync: press the real fob several times, then send from the emulator → still works.
  5. (If D shipped) Back → Save fob → re-open → resumes at the advanced counter.

## Non-goals

- No per-manufacturer button-count detection (fixed 4-tile grid; captured one marked).
- No editing of button labels in v1 (fixed Lock/Unlock/Trunk/Panic).
- No change to the clone crypto/counter engine.
- No public/hardened exposure.

## Rollback

Additive: the new view/scene and the two Clone-action route changes revert in one commit; SetJump + generic Transmitter remain intact as the fallback path.
