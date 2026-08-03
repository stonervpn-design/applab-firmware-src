# VAG Save + No-Desync Clone/Emulate — Design

**Date:** 2026-08-04
**Branch:** `feat/keeloq-no-desync-clone` (extends the existing KeeLoq clone/fob work)
**Status:** Approved (design), pending implementation plan

## Goal

Let a decoded **VAG** (VW/Audi/Seat/Skoda, AUT64 cipher) fob be **saved** to the Garage
and **emulated** on-screen as a fob — the same experience the KeeLoq no-desync clone
already provides — using the VAG decoder's fields and the port's existing VAG encoder.

## Motivation (from debugging)

- The port's VAG keystore is verified healthy on both devices; the user's own
  `VAG_032.sub` decodes (`Serial 130611555, Cnt 20045, KeyIdx 2`).
- "Can't save/clone" is **not** a key problem — it's that the Save/Clone button is
  hard-gated to KeeLoq (`can_clone` in `subghz_scene_rolling_analyzer.c:65` rejects any
  protocol whose name != `KeeLoq`). VAG never gets a Save button, and the fob view only
  knows KeeLoq buttons.
- The VAG encoder is already compiled (`ENABLE_EMULATE_FEATURE` defined in
  `lib/subghz/defines.h:8`) and **re-encrypts from scratch**: `vag_aut64_encrypt()`
  builds plaintext from serial+cnt+button and AUT64-encrypts at `KeyIdx`. So a
  counter-advancing clone (not just replay) is feasible.

## Non-Goals

- No public-build support. This rides the same private-only clone/Car-Keys surface that
  still lacks `PUBLIC_BUILD` guards; it stays private-only (see guardrail memory).
- No new cipher work — reuse the existing AUT64 encoder as-is.
- No replay-only mode (user chose no-desync clone).

## Architecture

**Generalize the existing KeeLoq machinery to be protocol-aware** rather than duplicate
it. Four touch points:

### 1. Save gate — `subghz_scene_rolling_analyzer.c`
- Rename intent of `can_clone` → **`can_save`**: return true when the protocol is
  `KeeLoq` **or** `VAG` (`VAG_PROTOCOL_NAME`). Keep the KeeLoq branch behavior identical.
- Save branch, VAG case: the decoded VAG instance already carries every field a `.sub`
  needs (`Serial/Btn/Cnt/KeyIdx/Type/Key/Key2`). Save it **directly** with
  `subghz_save_protocol_to_file` to `EXT_PATH("subghz/garage/<make>_<serial>.sub")` —
  no `gen_*_clone` step (that is KeeLoq-specific). `<make>` derived from the VAG type
  byte (VW/Audi/Seat/Skoda/VAG); `<serial>` zero-padded hex.
- After save, set `keeloq_fob_from_garage = true` and route to the fob scene (same as
  KeeLoq).

### 2. Protocol-aware fob view — `views/subghz_keeloq_fob.{c,h}`
- Add a **protocol tag** + **active button table** to the model. Two static tables:
  - KeeLoq: `{0x1,0x2,0x4,0x8}` / `{LOCK,UNLOCK,TRUNK,PANIC}` (4 tiles) — unchanged.
  - VAG: `{0x1,0x2,0x4}` / `{UNLOCK,LOCK,BOOT}` (3 tiles) — matches `vag_button_name`
    (0x1=Unlock, 0x2=Lock, 0x4=Boot).
- `subghz_keeloq_fob_set_state()` gains a **protocol/kind argument** (or a paired
  `_set_kind()` call) so the view selects the table + tile count.
- Draw + nav become **count-driven** (`n_tiles`), not hardcoded to 4. Nav generalizes to
  cycle/step over the active tiles (M5Stick tap-cycle already linear; Cardputer arrows
  step within the active count; a 3-tile layout renders as a row or 2+1).
- `subghz_keeloq_fob_selected_nibble()` returns the code from the **active** table.
- On-screen: a one-line **desync warning** for VAG (e.g. "adv counter — may desync fob").

### 3. Fob scene VAG branch — `subghz_scene_keeloq_fob.c`
- On enter, detect protocol from the loaded `fff_data` (read `Protocol`).
- **VAG path:** read `Serial/Btn/Cnt/KeyIdx/Type` from `fff`; set fob kind = VAG; build
  no-desync state — first emulate **jumps** the counter ahead (reuse
  `subghz->keeloq_clone_jump`, default 100), then **+1 per press**.
- On press: set `Btn` (active-table code) + advanced `Cnt` into `fff`, re-serialize,
  `subghz_tx_start`. The VAG encoder re-encrypts each frame at the saved `KeyIdx`.
  Counter refresh after TX completes, same tick logic as KeeLoq.
- **KeeLoq path:** unchanged (still builds via `subghz_txrx_gen_keeloq_clone`).

### 4. Garage routing — `subghz_scene_garage.c`
- Already generic (lists any `.sub`, `subghz_key_load` + routes to the fob scene). No
  change beyond the fob scene now branching on protocol. VAG `.sub`s appear and re-open
  with the counter resumed.

## Counter mechanic (no-desync, VAG)

- First emulate: `cnt_start = captured_cnt + jump` (jump default 100, reuses the KeeLoq
  clone jump state).
- Each press: `cnt += 1`, re-encrypt, TX. (VAG is a plain incrementing counter; exact
  step / whether frames pair like KeeLoq will be confirmed from the encoder's `yield`
  during implementation — default is a single frame, +1.)
- Garage re-open resumes from the last saved/emitted `Cnt`.

## Risks / Safeguards

- ⚠️ **Desync** (user-accepted): advancing the rolling counter can desync the genuine
  fob if VAG's accept-window differs from the +jump assumption. Mitigate with an
  on-screen warning; recovery = press the real fob a few times. Keep the jump modest.
- 🔒 **Private-only**: no `PUBLIC_BUILD` guard added here (tracked debt for the whole
  clone/Car-Keys surface); do not ship on any public build/webflasher.
- **Both devices**: changes live in `src_m5stick_s3`; must build + flash M5Stick (COM,
  by MAC) and rebuild the Cardputer bin (SD already has VAG keys). Mirror into
  `src_cardputer_adv` source tree is tracked debt (as with the KeeLoq clone).

## Testing

- **Host-testable pure logic:** the generalized N-tile nav (cycle/step over `n_tiles`),
  and the make/serial filename derivation from the VAG type byte — unit-test off-device.
- **Build gate:** ninja build of `src_m5stick_s3` green (both the M5Stick target and the
  Cardputer target).
- **On-device (user):** decode a VAG capture → Save appears → saved to Garage → open fob
  → 3 VAG tiles (Unlock/Lock/Boot) → press each → TX fires, counter advances, SENT
  shows; re-open from Garage resumes counter. (Hardware TX acceptance is user-verified.)

## Files

- Modify: `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c`
- Modify: `applications/main/subghz/views/subghz_keeloq_fob.c` + `.h`
- Modify: `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c`
- Verify (likely no change): `applications/main/subghz/scenes/subghz_scene_garage.c`
- Test: host test for nav + filename logic (pure functions)
