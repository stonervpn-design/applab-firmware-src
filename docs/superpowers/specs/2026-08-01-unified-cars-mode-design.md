# Unified "Cars" mode + decode dedup — design

**Date:** 2026-08-01
**Status:** Approved (design), pending implementation
**Boards:** `src_m5stick_s3` and `src_cardputer_adv` (private builds only)

## Problem

The SubGhz automotive decoder set is split into three resident registries the user
must manually swap between in `Radio Settings → Automotive`:

- **Off** (general): native gate/garage/TPMS set (~66 decoders)
- **Cars 1** (group A, ~34): first automotive batch, includes `hyundai_tl`
- **Cars 2** (group B, ~33): the rest, includes `jeep_v0`, `hitag2`, `honda_v2..v10`

Pain points:

1. **Constant swapping.** To decode an unknown car you may try Cars 1, get nothing,
   then switch to Cars 2. A Cars-2 car is silently undecodable while you sit in Cars 1.
2. **False decodes.** A Hyundai TL transmission decoded on another unit in Cars 2 as
   "jeep_v0 ×3, hitag2 ×1" — loose fixed-code decoders grabbing a signal that isn't theirs.
   This is a pre-existing bug within a single group; it is not caused by the split.

## Root-cause finding (why the split is unnecessary)

The split was justified in-code by: *"allocating all 62 (each ProtoPirate decoder carries
its own large capture buffer) exhausts the Cardputer ADV heap."* **This is stale and wrong
for the current decoders.** It was inherited from the original heavyweight ProtoPirate app
decoders and never re-validated after they were reimplemented as lean native ports in
`lib/subghz/protocols/`.

Measured (static scan of every car decoder struct, `audit`/sizeof of in-struct arrays):

| Metric | Value |
|--------|-------|
| Sum of all 61 car-decoder in-struct buffer bytes | **~5.2 KB** |
| Largest single decoder (`hitag2`: `int16_t durs[700]` + `all_bits[400]`) | 1.8 KB |
| Most decoders | 0 buffer bytes (small fixed fields only) |
| Est. total resident incl. ~250 B/struct overhead × 61 | **~20-25 KB** |
| Free heap on board | ~103 KB |
| General mode already-resident decoder count (works today) | ~66 |

Conclusion: all 61 car decoders fit resident **simultaneously** with ~75-80 KB to spare.
The general set already proves a set this size fits. Memory is a non-issue.

## Goal

One automotive mode. Select **Cars** and any supported car decodes immediately with no
manual group switching — and without the jeep/hitag-style false decodes.

## Non-goals

- No smart-fallback / dwell-timer / time-slicing (considered and rejected: it fits in RAM,
  so time-slicing would only add latency and RX gaps for no benefit).
- No changes to public/hardened builds' behavior (car decoders remain stripped there).
- No rewrite of individual decoder state machines (dedup handles collisions generically).

## Design

### A. Merge the automotive registries

`lib/subghz/protocols/protocol_items.c` / `.h`:

- Replace the two arrays `subghz_protocol_registry_items_automotive_a[]` and
  `...automotive_b[]` with a single `subghz_protocol_registry_items_automotive[]`
  containing all 61 unique car decoders + one `raw` + one `bin_raw`.
  Keep the `#ifndef PUBLIC_BUILD` guard; the `#else` branch stays raw/bin_raw-only.
- `car_mode` semantics collapse from `{0,1,2}` to `{0 = Off/general, 1 = Cars}`.
  - `subghz_protocol_registry_set_car_mode(mode)`: clamp `mode >= 1` to `1`
    (this transparently maps a stale saved `car_mode == 2` to `Cars`).
  - `subghz_protocol_registry_active()`: `case 1 → &..._automotive; default → general`.
- Delete `subghz_protocol_registry_automotive_b`, the current-group state, and the
  stale "won't fit / split" comment. Update the comment to record the real footprint.

`applications/main/subghz/helpers/subghz_txrx.c`:

- `subghz_txrx_set_car_mode()` logic is unchanged (it already frees + rebuilds the
  receiver from `registry_active()` on change, RX stopped). It simply never receives `2`.

`applications/main/subghz/scenes/subghz_scene_radio_settings.c`:

- `car_mode_text[]` → `{"Off", "Cars"}`, `CAR_MODE_COUNT` follows automatically.
- The stale "split in two / no-PSRAM" comment on the setter is updated.

### B. Decode dedup / rank

New isolated unit (e.g. `applications/main/subghz/helpers/subghz_decode_dedup.{c,h}`),
owned by the `SubGhz` app instance, with state:

```
typedef struct {
    const char* last_protocol;   // protocol name pointer of last committed result
    uint16_t    last_score;      // score of last committed result
    uint32_t    last_tick;       // furi_get_tick() of last committed result
    int32_t     last_history_idx;// history index of last committed result (-1 = none)
} SubGhzDecodeDedup;
```

Scoring (`subghz_decode_dedup_score(decoder_base)`):

- Primary: a small static priority table keyed by protocol name/pointer.
  - **Low** (loose fixed-code prone to false hits): `jeep_v0`, `hitag2`, and any other
    known-loose decoders identified during implementation.
  - **High** (specific / rolling / checksum-validated): `hyundai_tl`, `keeloq`,
    `secplus_v1/v2`, etc.
  - **Medium**: default for everything not listed.
- Tiebreak: decoded bit-count (`data_count_bit`) when readily available; higher = better.

Integration in `subghz_scene_add_to_history_callback()` (`subghz_scene_receiver.c`),
**before** `subghz_history_add_to_history()`:

- `now = furi_get_tick()`; `score = score(decoder_base)`.
- If `last_history_idx >= 0` and `now - last_tick <= DEDUP_WINDOW` (≈150 ms):
  - `score > last_score` → **replace**: `subghz_history_delete_item(last_history_idx)` +
    matching `subghz_view_receiver_delete_item`, then fall through to add the new one and
    update dedup state to the new result. (Handles the loose-decoder-finishes-first case.)
  - `score <= last_score` → **suppress**: skip the add entirely (still call
    `subghz_receiver_reset`). Do not update `last_tick` (window anchored to the winner).
- Else (no recent result) → normal add, then set dedup state to this result.

The window is time-only (same-burst decoders complete within tens of ms; frequency is
constant in non-hopper RX). No serialization needed — history already snapshots the decode,
and `delete_item` already exists (used by the delete-old-signals path).

Reset dedup state (`last_history_idx = -1`) on scene enter and whenever history is cleared.

### C. Scope / guards

- Both trees (`src_m5stick_s3`, `src_cardputer_adv`). Car decoders and the dedup priority
  entries for them live under `#ifndef PUBLIC_BUILD`; the dedup unit itself compiles in all
  builds (it is protocol-agnostic and harmless with only raw/bin_raw present).
- Public/hardened: `Automotive = {Off, Cars}` with Cars exposing raw-only (unchanged behavior,
  just one fewer menu entry).

## Components & boundaries

- **Merged registry** (`protocol_items.*`): what decoders are resident. Interface unchanged
  (`registry_active`, `set_car_mode`, `car_mode`), semantics narrowed to 0/1.
- **TxRx helper**: rebuilds the receiver on mode change. Untouched interface.
- **Radio-settings scene**: presents `{Off, Cars}`. Depends only on `set/get_car_mode`.
- **Dedup unit**: pure decision over `(protocol, score, tick)` → keep/replace/suppress.
  Testable in isolation from a sequence of synthetic `(protocol, tick)` events.
- **Receiver scene**: calls the dedup unit around the existing history add.

## Data flow

RX pulse stream → `subghz_worker` → `subghz_receiver_decode` (all resident decoders) →
per-decoder completion → `subghz_scene_add_to_history_callback` → **dedup decision** →
`subghz_history_add_to_history` / `delete_item` → receiver view menu.

## Error handling / edge cases

- Stale saved `car_mode == 2` → clamped to `1` (Cars). No migration file needed.
- Replace path must delete from **both** history and the view menu to stay consistent
  (mirror the existing delete-old-signals block).
- Hopper mode: dedup window is time-based; frequency changes between bursts are far apart
  (> window), so no cross-frequency mis-merge. No interaction with the (now-removed) group
  swap.
- `delete_item` on the just-added index: reuse the exact API sequence already proven in the
  delete-old-signals path to avoid view/history index skew.

## Testing

Per-tree, built separately (pioarduino one-env-at-a-time rule), flashed m5stick COM10 /
cardputer COM12:

1. **No-swap decode:** In **Cars**, decode a Hyundai TL (old group A) *and* a Jeep/Honda
   (old group B) back-to-back without touching the mode. Both appear.
2. **Dedup:** TX a Hyundai TL; confirm a single "Hyundai TL" history entry, not
   "jeep ×3 + hitag ×1".
3. **Replace ordering:** verify a case where a loose decoder would finish first still ends
   with the specific decode shown (delete+replace path).
4. **Heap:** watch serial free-heap across many RX cycles and Off↔Cars toggles; confirm flat
   (no per-rebuild leak) and comfortably above headroom.
5. **Regression:** Off (general) mode decodes native protocols unchanged; public build shows
   `{Off, Cars}` with raw-only Cars and links clean.

## Rollback

Single-commit revert restores the three-registry split and 3-state selector; the dedup unit
is additive and can be disabled by not calling it in the callback.

## Related

- `2026-07-15-ram-shrink-on-demand-buffers-design.md` (the buffer-shrink pattern that would
  have been needed *if* the merge didn't fit — it does, so it is not used here).
- Hyundai TL decoder/encoder work (`hyundai_tl.{c,h}`), the source of the observed collision.
