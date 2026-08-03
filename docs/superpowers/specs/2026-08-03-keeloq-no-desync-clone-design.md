# KeeLoq No-Desync Clone — Design Spec

**Date:** 2026-08-03
**Status:** Design (pending user review)
**Boards:** `src_m5stick_s3` and `src_cardputer_adv` (private builds only)
**Prerequisite:** a known manufacturer master key (Kmfg) present in the keystore for the
target — i.e. one of the 176 plaintext `keeloq_mfcodes_user` entries (SIMPLE / NORMAL /
MAGIC / AERF / JCM / etc.). SECURE-learning mfrs (BFT, Erreka) additionally need a seed.

## Goal

Add an **algorithmic "no-desync" KeeLoq clone**: a stored fob identity (derived device key
`kdev` + serial + last counter) that transmits **synthesized future counter frames** so the
target receiver always accepts and never desyncs — even if the real fob has been used since
the clone was made. Unlike a captured `.sub` replay (stale counter → rejected once the real
fob moves ahead), the clone holds `kdev` and can mint a cryptographically valid frame for any
counter value, always jumping ahead of the real fob.

## Key insight (from the subsystem map)

The existing "KL: By Manufacturer" engine **already derives `kdev` and builds valid frames**.
No new crypto is required. "No-desync" decomposes into two additions on top of the existing
engine plus wiring:

- `subghz_protocol_keeloq_gen_data` (`keeloq.c:145`) already builds `fix=(btn<<28)|serial`,
  packs the 16-bit counter into the hop plaintext, derives `kdev` via
  `switch(manufacture_code->type)` (`keeloq.c:439`), and encrypts
  (`subghz_protocol_keeloq_common_encrypt`, `keeloq_common.c:16`).
- The **only** desync source is the per-send drift `cnt += rolling_counter_mult`
  (`keeloq.c:255`, counter_up path gated at `:245`).
- A back-to-back **(N, N+1) pair** emitter already exists as counter-mode 7
  (`keeloq.c:678-702`, N at `:688`, N+1 at `:693`) — reuse it for double-window resync.
- After any live decode, `serial` (`keeloq.c:1400`), `cnt` (`:983`), `btn` (`:1401`) and the
  matched manufacturer are already on the instance — a 1:1 feed into `create_data`
  (`keeloq.c:539`) / `subghz_txrx_gen_keeloq_protocol`
  (`subghz_txrx_create_protocol_key.c:96`), so **clone-from-capture needs zero manual entry**.
- The `.sub` format stores only `Key` + `Manufacture` *name* (`generic.c:198`, `keeloq.c:1448`);
  serial/btn/counter live inside the encrypted `Key` and `kdev` is re-derived from the
  keystore on load. A resumable, keystore-independent clone therefore needs **new fields**.

## Confirmed design decisions

1. **Counter strategy — Jump + resync pair.** Each send computes
   `target = max(stored_last_cnt, seen_cnt) + jump` (user-settable `jump`, default 100) and
   emits the pair `[target, target+1]` back-to-back in one upload buffer, then persists
   `target+1`. Monotonic forward advance → never behind → never desyncs. The pair satisfies
   both single- and double-operating-window receivers.
2. **Store Kdev in the clone.** The derived 64-bit device key is persisted in the `.sub`, so
   the clone transmits correctly even if the manufacturer key is absent/renamed in the
   on-device keystore. Fully self-contained.
3. **Private builds only.** `src_m5stick_s3` + `src_cardputer_adv`, under
   `#ifndef PUBLIC_BUILD`. Public/hardened builds unchanged.

## Design

### A. No-desync TX engine (`lib/subghz/protocols/keeloq.c`)

Introduce a **clone TX mode** that (a) uses a stored `kdev` directly and (b) emits the
forward pair instead of drifting.

- **Kdev override.** Add `instance->clone_kdev` (uint64) + `instance->clone_active` (bool).
  When `clone_active`, `gen_data` skips the `manufacture_code` keystore scan
  (`keeloq.c:431-524`) and computes `hop = encrypt(decrypt_plaintext, clone_kdev)` directly.
  The `decrypt` plaintext assembly (`keeloq.c:350`) and `fix` assembly (`:204`) are unchanged;
  only the `kdev` source changes. This isolates the clone from keystore presence.
- **Forward-pair counter mode.** Gate on the **per-instance `clone_active` flag** (not the
  file-static `keeloq_counter_mode` global — see edge cases for why) so that, on `get_upload`
  (`keeloq.c:665`), the clone path emits exactly two
  frames via `encode_to_timings(instance, btn, counter_up=false, ...)` (`keeloq.c:603`) with
  `instance->generic.cnt` pinned to `target` then `target+1` — modeled on the mode-7 block
  (`keeloq.c:678-702`) but trimmed to 2 frames and driven by the clone counter. `counter_up`
  is forced **false** so no implicit drift (`keeloq.c:245` gate is bypassed).
- **Counter advance algorithm** (owned by the clone unit, not `gen_data`). The initial `jump`
  establishes the lead over the real fob **once**; every subsequent send advances by the pair
  width (+2) only, conserving 16-bit counter space:
  ```
  state: stored_last_cnt (persisted), synced (persisted bool), jump (default 100)
         synced starts false at clone build; resets to false on a fresh re-capture

  on send:
    if not synced:
        seen_cnt = counter from an optional fresh re-capture of the live fob,
                   else stored_last_cnt
        target   = (max(stored_last_cnt, seen_cnt) + jump) & 0xFFFF   ; one-time lead
        synced   = true
    else:
        target   = (stored_last_cnt + 1) & 0xFFFF                     ; +2 pair advance
    emit frame @ target
    emit frame @ (target + 1) & 0xFFFF        ; same upload buffer, no RF gap
    stored_last_cnt = (target + 1) & 0xFFFF   ; persist stored_last_cnt AND synced
  ```
  16-bit wrap is modular; the double-window pair resyncs across a wrap. One-time jump then
  +2/send means the counter advances ~2 per press after the first — thousands of presses
  before wrap; wrap is handled, not an error. Re-capturing the live fob clears `synced` so the
  `jump` lead is re-applied over any fresh real-fob usage.

### B. Clone persistence (`keeloq.c` serialize/deserialize + `.sub` format)

Extend the KeeLoq `.sub` with explicit clone fields so the identity is self-contained and the
counter survives reloads:

```
Filetype: Flipper SubGhz Key File
Version: 1
...
Protocol: KeeLoq
Bit: 64
Key: <current 64-bit frame for the last target>   # still written for compatibility/display
Serial: <28-bit hex>          # NEW
Btn: <4-bit>                  # NEW
Cnt: <16-bit last_last_cnt>   # NEW  (clone's persisted last-sent counter)
Kdev: <64-bit derived key>    # NEW  (device key; enables keystore-independent TX)
LearnType: <KEELOQ_LEARNING_* int>   # NEW (for display + optional re-derive)
CloneJump: <uint16, default 100>     # NEW (user-set one-time lead)
CloneSynced: <0|1>            # NEW (has the one-time jump lead been applied yet)
Manufacture: <name>           # existing (kept for display; TX no longer depends on it)
CloneMode: 1                  # NEW sentinel marking this as a no-desync clone
```

- **Serialize** — extend `subghz_protocol_decoder_keeloq_serialize` (`keeloq.c:1420`), writing
  the new fields after the existing `Manufacture` write (`keeloq.c:1447-1448`) when
  `clone_active`.
- **Deserialize (TX path)** — extend `subghz_protocol_encoder_keeloq_deserialize`
  (`keeloq.c:712`) to read the new fields (mirror site `keeloq.c:774` for `CounterMode`). When
  `CloneMode` is present: set `clone_active=true`, load `clone_kdev`, `serial`, `btn`,
  `generic.cnt=Cnt`, `jump`, `synced`, and **bypass** the keystore-name derivation at
  `check_remote_controller` (`keeloq.c:1327`) — the clone decrypts/encrypts with `Kdev`.
- **Counter write-back** — after each send, rewrite `Cnt` in the clone's `fff_data` (pattern:
  the encoder already rewrites `Key` at `keeloq.c:806`), so re-opening resumes forward.

### C. Entry points (scenes)

Two ways to create a clone; both converge on the same clone builder and transmitter.

- **From a capture (primary, zero manual entry).** In the received-signal action scene
  `subghz_scene_receiver_info.c` (which already gates actions via `can_save`/`can_send` at
  `:162-163` and routes to Save/Analyze/TpmsEdit), add a **"Make no-desync clone"** action —
  gated by a new `can_clone` predicate (KeeLoq + encodable learning type; not `Unknown`, not
  seed-only). It reads the
  decoded `serial`/`cnt`/`btn`/mfr off the instance, derives `kdev` once via the matching
  learning fn (`keeloq_common.c`), and builds the clone. Grayed out / hidden for `Unknown`
  and SECURE (BFT/Erreka) captures (no seed).
- **From By-Manufacturer.** Add a third item **"Clone (no-desync)"** to the KeeLoq Action
  submenu (`subghz_scene_keeloq_action.c:41`, enum `:5-8`, callback `:10`), beside Emulate /
  Save. This scene only runs when `keeloq_mfr_emulate_flow==true`, so the branch is naturally
  scoped. Uses the manually entered serial/btn/counter + selected Kmfg.
- **Jump setting.** A small byte-input scene (modeled on `subghz_scene_set_counter.c`) to set
  `CloneJump` before first send; default 100, persisted in the clone.
- **Transmit.** Reuse the Transmitter scene (`subghz_scene_transmitter.c`) with `clone_active`
  set: OK sends the forward pair and advances+persists the counter; the D-pad custom-button
  remap still works via `subghz_protocol_keeloq_get_btn_code` (`keeloq.c:1534`). The tick-time
  restore path (`subghz_scene_transmitter.c:111-123`) must be counter-frozen for the clone
  (it already forces `rolling_counter_mult=0`; verify it does not double-advance the clone).

## Components & boundaries

**Modify (both private trees):**
- `lib/subghz/protocols/keeloq.c` — clone kdev override in `gen_data`; forward-pair emit in
  `get_upload`; serialize/deserialize new fields; load-time keystore bypass.
- `lib/subghz/protocols/keeloq.h` — declare clone API (`..._create_clone_data(...)`,
  `..._set_clone(instance, kdev, serial, btn, cnt, jump)`).
- `applications/main/subghz/helpers/subghz_txrx_create_protocol_key.{c,h}` — a
  `subghz_txrx_gen_keeloq_clone(...)` that populates `fff_data` incl. the new fields
  (extends the `Manufacture` write at `subghz_txrx_create_protocol_key.c:121`).
- `applications/main/subghz/scenes/subghz_scene_keeloq_action.c` (+ its scene enum) — third
  menu item.
- `applications/main/subghz/scenes/subghz_scene_receiver_info.c` — "Make no-desync clone"
  action + `can_clone` predicate (gated like `can_save`/`can_send` at `:162-163`).
- `applications/main/subghz/subghz_i.h` — app-struct fields: clone flow flag + pending
  `clone_jump`, mirroring the existing `keeloq_mfr_emulate_flow` clear-sites.

**Create (both private trees):**
- `applications/main/subghz/scenes/subghz_scene_keeloq_clone.c` (+ register in
  `subghz_scene_config.h`) — the clone build/confirm scene (set jump → build → route to
  Transmitter). Small, single-purpose.

**Unit boundaries** — each independently reasoned/tested:
- *Clone counter unit*: pure `(stored_last_cnt, seen_cnt, jump) → (target, target+1, new_last)`.
  Host-testable with no hardware (add to the existing keeloq host-test harness).
- *Kdev override in gen_data*: given `(serial, btn, cnt, kdev)` → deterministic 64-bit frame;
  verify against a known keystore-derived frame (same output when kdev matches the derived key).
- *Serialize/deserialize*: round-trip a clone `.sub` → fields preserved, counter resumes.
- *Scenes*: routing only; depend on the above through the txrx helper.

## Error handling / edge cases

- **Seed-only mfrs (SECURE: BFT, Erreka).** Clone-from-capture is not possible (a normal press
  carries no seed). Hide/disable the capture entry for these; the By-Manufacturer path with
  explicit seed entry still works and stores the seed alongside `Kdev`.
- **Unknown mfr / no keystore match.** `manufacture_name=="Unknown"` → `cnt` forced 0
  (`keeloq.c:1314-1316`); no rolling clone possible — only a raw fixed replay. Disable the
  clone action for `Unknown`.
- **Counter wrap (0xFFFF→0).** Modular; the pair resyncs across the wrap. Persist `& 0xFFFF`.
- **`keeloq_counter_mode` / `bypass` are file-static globals** (`keeloq.c:60/19`) read again on
  the next signal. The clone mode must set/reset them per-send (or prefer the per-instance
  `clone_active` flag over the global) to avoid leaking state into the next transmission
  (mirrors the cross-contamination risk noted for `CounterMode`).
- **`keeloq_mfr_emulate_flow` clear-sites.** Any new parallel flow flag must be cleared at the
  same sites (`set_type.c:106`, `keeloq_action.c:18/23/67`) or a back-out leaves the flow armed.
- **Static mfr-name buffer** (`subghz_keeloq_mfr.c:37`) is shared; the clone must copy the
  manufacturer name it needs rather than hold the pointer.
- **Ambiguous auto-detected key.** With an empty/short discriminator the first validating
  keystore key wins (`keeloq.c:1052-1054`); the capture flow should surface which Kmfg matched
  and let the user confirm before committing the clone.

## Testing

Built per-tree (pioarduino one-env-at-a-time), flashed m5stick COM10 / cardputer COM12.

1. **Host unit tests:** clone counter unit (jump/pair/wrap), kdev-override frame equality vs
   keystore-derived frame, `.sub` round-trip.
2. **Compile gate:** `build_ci.bat` per private tree; confirm `#ifndef PUBLIC_BUILD` guards
   compile the public tree clean without the clone.
3. **Clone-from-capture:** capture a NORMAL-learning fob press → "Make no-desync clone" →
   confirm serial/counter/mfr auto-filled, `Kdev` stored → transmit → target device actuates.
4. **No-desync proof:** after cloning, press the **real fob several times** (advance its
   counter), then transmit the clone → device still actuates (clone jumped ahead; the pair
   resynced). Re-open the saved clone → counter resumes forward, still works.
5. **By-Manufacturer path:** KL: By Manufacturer → serial/btn/counter → "Clone (no-desync)" →
   set jump → transmit; Emulate/Save unchanged.
6. **Guards / regression:** SECURE (BFT) capture hides the clone action; `Unknown` capture
   hides it; normal Emulate and Save behave exactly as before; public build links clean and
   shows no clone UI.

## Non-goals

- No change to the existing Emulate or Save behavior.
- No new crypto / no new learning types — reuse the existing derivation switch.
- No public/hardened exposure.
- No cracking of unknown keys — the clone requires a **known** Kmfg (prerequisite).

## Rollback

Additive. A single revert of the clone commit restores stock behavior: the new `.sub` fields
are ignored by the stock loader (unknown keys skipped), the third menu item and the capture
action disappear, and `gen_data`/`get_upload` fall back to the keystore-derived, drifting path.

## Related

- `2026-07-10-keeloq-emulate-design.md` — the Emulate routing this extends.
- `2026-08-01-unified-cars-mode-design.md` — car-feature private-build precedent.
- `keeloq-psa-decoder-overhaul` memory — the AERF `nl_extend` shift-UB fix; the clone's
  AERF path depends on that corrected derivation.
- The 176-key `kmfg_derivation_table.csv` — the Kmfg → learning-type map the clone relies on.
