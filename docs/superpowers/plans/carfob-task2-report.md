# Car-fob emulate-all-protocols — Task 2 report

**Task:** Generalize the multi-button fob VIEW (`views/subghz_keeloq_fob.{c,h}`) so it can
render/navigate an arbitrary `SubGhzFobButton[]` table, with VAG/KeeLoq unchanged. VIEW ONLY —
no scene/routing/TX changes (Task 3).
**Branch:** feat/keeloq-no-desync-clone
**Status:** DONE. Private cardputer build GREEN. Committed (no push).

## What changed
The view is now fully table-driven. The old model held a `SubGhzFobKind kind` and branched on
it in three places (title, VAG desync note, counter width) plus a `fob_tiles(kind,...)` lookup
that returned one of two hardcoded `{codes[],labels[]}` pairs. That is replaced by a single
model that stores a **pointer + count** to a static `SubGhzFobButton[]` table plus three
presentation fields, so there is exactly ONE render/nav path:

```c
typedef struct {
    uint8_t sel, n_tiles, real_btn;
    uint32_t cnt; uint16_t jump; bool sent;
    bool wide_cnt;                  // VAG 24-bit (%06lX) vs 16-bit (%04lX)
    bool desync_caution;            // VAG "adv cnt / desync!" note in the empty 4th cell
    const SubGhzFobButton* buttons; // static provider table, NOT owned, no copy
    const char* title;              // static string, NOT owned
} SubGhzKeeloqFobModel;
```

All public setters funnel through one internal `fob_apply_table(instance, buttons, n, title,
wide_cnt, desync_caution, real_btn, cnt, jump)` which clamps `n` to `SUBGHZ_KEELOQ_FOB_MAX_TILES`
(8), derives the initial `sel` from `real_btn`, and stores only the pointer (no heap copy — safe
for the no-PSRAM Cardputer).

## New / changed API (`subghz_keeloq_fob.h`)
```c
#define SUBGHZ_KEELOQ_FOB_MAX_TILES 8

// NEW — generic entry point. buttons must be static-lifetime (e.g. from
// subghz_fob_buttons()); only pointer+count stored, n clamped to the max.
void subghz_keeloq_fob_set_table(
    SubGhzKeeloqFob* instance, const SubGhzFobButton* buttons, size_t n,
    uint8_t real_btn, uint32_t cnt, uint16_t jump);

// NEW — override the top title (static string, e.g. a protocol name). set_table
// defaults the title to "Fob Emulator"; Task 3 calls this for generic protocols.
void subghz_keeloq_fob_set_title(SubGhzKeeloqFob* instance, const char* title);

// UNCHANGED signature — now a thin wrapper over fob_apply_table (see below).
void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance, SubGhzFobKind kind,
    uint8_t real_btn, uint32_t cnt, uint16_t jump);
```
The header now `#include`s `../helpers/subghz_fob_buttons.h` (for `SubGhzFobButton` / `size_t`).
`subghz_keeloq_fob_selected_nibble` and `subghz_keeloq_fob_set_sent` are unchanged in signature;
`selected_nibble` now reads `m->buttons[m->sel].code` (bounds-checked) instead of the old
`fob_tiles` lookup. `set_sent` is byte-for-byte unchanged as required.

## How VAG/KeeLoq stay pixel-identical
`set_state` is now a thin wrapper — it looks up the two legacy tables from the Task-1 provider
by the protocols' own name macros and calls the shared path:
- **VAG** → `subghz_fob_buttons(VAG_PROTOCOL_NAME, &n)` = `{0x1 UNLOCK, 0x2 LOCK, 0x4 BOOT}`
  (identical to the old `VAG_CODES/VAG_LABELS`), `wide_cnt=true`, `desync_caution=true`,
  title `"VAG Fob"` (`#ifndef PUBLIC_BUILD`) / `"Fob Emulator"` (public) — same `#ifdef` as before.
- **KeeLoq** → `subghz_fob_buttons(SUBGHZ_PROTOCOL_KEELOQ_NAME, &n)` =
  `{0x1 LOCK, 0x2 UNLOCK, 0x4 TRUNK, 0x8 PANIC}` (identical to old `FOB_CODES/FOB_LABELS`),
  `wide_cnt=false`, `desync_caution=false`, title `"Fob Emulator"`.

The draw code preserves every coordinate. `fob_tile_rect(i, n, ...)` returns, for **n<=4**, the
exact legacy 2x2 geometry (x = `(i&1)?66:2`, y = `(i<2)?13:35`, 60x18), so:
- box `rbox/rframe(x,y,60,18,3)` — identical
- label at `x+w/2, y+h/2` = `x+30, y+9` — identical
- real-btn disc at `x+w-6, y+4` = `x+54, y+4` — identical
- VAG desync note (now gated on `m->desync_caution`) at the same `(66+30, 35+5/35+14)`
- counter line at `(64,63)` with the same `%06lX`/`%04lX` format strings.

Provider tables are static const and their codes/labels were transcribed from the same maps the
view used, so VAG (3 tiles) and KeeLoq (4 tiles) render byte-for-byte as before. `n>4` uses a
denser 2-column layout (60x11) purely so `set_table` can't index out of bounds; no VAG/KeeLoq
path ever takes it.

## Build result
`idf.py -B build_cardputer_adv build` (private cardputer, PowerShell, incremental) →
**Project build complete**, exit code 0. No warnings/errors on the two changed files.

## Concerns / notes for Task 3
- **Provider is now linked from the view** (not just host-tested). `helpers/subghz_fob_buttons.c`
  is globbed into the subghz app (`*.c*` default), so `subghz_fob_buttons()` links fine; the
  `fob_apply_table` guard `furi_check(buttons && n>0)` is a dev tripwire — VAG/KeeLoq always hit.
- **Nav for n>4 is NOT yet real.** `subghz_fob_nav_move_n` clamps `idx>3→0` in its `n>=4`
  branch (2x2 spatial), so tiles 4-7 of a 6-tile Honda table are drawn but unreachable by the
  arrows/cycle. VAG(3)/KeeLoq(4) are unaffected. Task 3 (or a small nav follow-up) must extend
  `subghz_fob_nav_move_n` + `fob_tile_rect` before routing a >4-button protocol into the grid.
- **`set_table` defaults** to title "Fob Emulator", 16-bit counter, no desync note. Task 3 should
  call `set_title(proto_name)` and decide per-protocol counter width if any generic protocol has
  a >16-bit counter (none of the 21 supported tables imply one today).
- No scene/TX changes were made; `subghz_scene_keeloq_fob.c` still calls `set_state` exactly as
  before, so KeeLoq clone + VAG emulate behavior is untouched.
