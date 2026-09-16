# Task 1 report — Button catalog + synth/replay audit

**Status:** DONE
**Commit:** `5c9971374634a491aa0e847fe6bc88e932dd2e02` (branch `feat/keeloq-no-desync-clone`, not pushed)

## Deliverables
- `applications_user/proto_pirate/protocols/protopirate_fob_buttons.h` — self-contained
  (no app/furi headers): `PPFobButton`, `pp_fob_buttons()`, `pp_fob_can_synthesize()`.
- `applications_user/proto_pirate/protocols/protopirate_fob_buttons.c` — 13 per-protocol
  `static const PPFobButton <proto>_buttons[]` tables + master registry + self-contained
  canonical-name/alias resolver (mirrors `protocol_items.c` aliases; not linked so the TU
  stays host-testable). Full audit table is the comment block at the top of the file.
- `tests/test_fob_buttons.c` — host test.

## Classification rule
- **SYNTH** (`pp_fob_can_synthesize == true`): encoder `deserialize` reads an *independent*
  button field (`FF_BTN` directly or via `pp_encoder_read_fields()` in `protocols_common.c`)
  **and** the protocol is not replay-only. A discrete selectable grid is returned by
  `pp_fob_buttons()` only when the protocol also exposes a code->name map.
- **REPLAY** (`pp_fob_can_synthesize == false`): encoder re-emits the captured raw frame with
  no independent button field. No catalog returned; emulate view falls back to captured button.

## Full audit (27 TX-capable protocols)

| Protocol | Class | Reads btn | Buttons in catalog |
|---|---|---|---|
| Chrysler V0 | SYNTH | pp_encoder_read_fields | 2 (Lock/Unlock) |
| Fiat V0 | SYNTH | pp_encoder_read_fields | 0 (range endbyte, no discrete map) |
| Fiat V1 | SYNTH | pp_encoder_read_fields | 4 (Close/Trunk/Lock/Unlock) |
| Fiat V2 | REPLAY | — (re-emits Raw) | 0 |
| Ford V0 | SYNTH | pp_encoder_read_fields | 4 (Panic/Lock/Unlock/Boot) |
| Ford V1 | SYNTH | pp_encoder_read_fields | 5 (Sync/Lock/Unlock/Trunk/Panic) |
| Ford V2 | REPLAY | — (re-emits Raw) | 0 |
| Ford V3 | REPLAY | — (re-emits Raw) | 0 |
| Honda Static | SYNTH | pp_encoder_read_fields | 6 (Lock/Unlock/Trunk/Remote Start/Panic/Lock x2) |
| Honda V1 | SYNTH | pp_encoder_read_fields | 4 (Unlock/Lock/Trunk/Panic) |
| Honda V2 | SYNTH | FF_BTN direct | 2 (Lock/Unlock) |
| Kia V0 | SYNTH | FF_BTN direct | 3 (Lock/Unlock/Trunk, Kia-default map) |
| Kia V1 | SYNTH | pp_encoder_read_fields | 3 (Close/Open/Boot) |
| Kia V2 | SYNTH | pp_encoder_read_fields | 0 (no discrete map) |
| Kia V3/V4 | SYNTH | pp_encoder_read_fields | 0 (no discrete map) |
| Kia V5 | SYNTH | pp_encoder_read_fields | 0 (no discrete map) |
| Kia V6 | SYNTH | pp_encoder_read_fields | 0 (no discrete map) |
| Kia V7 | SYNTH | FF_BTN direct | 4 (Lock/Unlock/Trunk/Trunk) |
| Mazda V0 | SYNTH | pp_encoder_read_fields | 4 (Lock/Unlock/Trunk/Remote) |
| Mitsubishi V0 | SYNTH | pp_encoder_read_fields | 0 (no discrete map) |
| Porsche Touareg | SYNTH | pp_encoder_read_fields | 0 (no discrete map, numeric Btn) |
| PSA | SYNTH | pp_encoder_read_fields | 0 (no discrete map, numeric Btn) |
| Renault V0 | REPLAY | field only on Type13 path; general path replays captured_button | 0 |
| Scher-Khan | REPLAY | — (re-emits data bits) | 0 |
| Star Line | SYNTH | pp_encoder_read_fields | 0 (no discrete map, KeeLoq-style) |
| Subaru | SYNTH | pp_encoder_read_fields | 4 (Lock/Unlock/Trunk/Panic) |
| VAG | SYNTH | pp_encoder_read_fields | 6 (Unlock/Lock/Boot, two encodings) |

Totals: **22 SYNTH / 5 REPLAY**; **13 protocols expose a discrete button grid** (78 button rows).
`aut64.c` is a pure AUT64 cipher helper (no encoder) — excluded.

## Test
Command (single-TU; the test `#include`s the `.c`, matching `tests/test_fob_nav.c` style):
```
wsl bash -lc 'cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && \
  gcc -Wall -Wextra -I. tests/test_fob_buttons.c -o /tmp/tfb && /tmp/tfb'
```
Result: **PASS** — `test_fob_buttons OK` (no warnings). Asserts: VAG >=4 buttons incl.
Lock/Unlock/Boot and codes 0x1/0x2/0x4; `VW` alias resolves to the same table;
unknown name and NULL name -> `(NULL, 0)`; `pp_fob_can_synthesize` true for VAG/VW,
false for Ford V3/Ford V2/Fiat V2/Scher-Khan and unknown/NULL; Subaru + Honda V1 spot-checks.

## Concerns / notes for later tasks
1. **VAG names are Unlock/Lock/Boot, not "Trunk/Panic".** `vag.c`'s `vag_button_name` only
   defines Unlock/Lock/Boot (British "Boot" = trunk); it has no Panic. The plan's "Trunk and
   Panic" wording for the VAG assert doesn't match the source, so the test asserts what VAG
   actually defines (per the plan's own "whatever VAG actually defines" caveat). VAG table has
   6 rows (0x1/0x2/0x4 and their 0x10/0x20/0x40 alternate encodings), so names repeat.
2. **SYNTH-but-no-grid protocols** (Fiat V0, Kia V2/V3-4/V5/V6, Mitsubishi, PSA, Porsche, Star
   Line): `pp_fob_can_synthesize` is `true` per the literal interface contract (reads a button
   field, not replay-only), but `pp_fob_buttons` returns `NULL` because they print the button
   numerically with no code->name map. Task 3/4 must handle "can_synthesize && buttons==NULL"
   (e.g. captured-button fallback or a numeric selector), otherwise the grid would be empty.
3. **Renault V0 classified REPLAY** (conservative): its encoder reads the button field only on
   the rolling Type13 path; the common path replays `captured_button` and its name map is
   range-based. If Task 4 wants Renault rolling synthesis, revisit this row.
4. **Kia V0 map is type-dependent** (Honda/Suzuki/Kia sub-maps via `kia_v0_button_name`). The
   catalog uses the Kia-default map {0x01 Lock, 0x02 Unlock, 0x03 Trunk}. Suzuki/Honda V0
   aliases currently resolve to that same Kia-default set; if the view needs the Suzuki/Honda
   variants, add per-alias tables.
5. **Alias table is duplicated** from `protocol_items.c` to keep the TU host-testable without
   furi. Keep the two in sync if catalog aliases change (noted in a comment in the `.c`).
