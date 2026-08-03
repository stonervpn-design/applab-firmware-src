# VAG Save + No-Desync Clone/Emulate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a decoded VAG (AUT64) fob be saved to the Garage and emulated on-screen as a no-desync clone, reusing the existing KeeLoq fob/Garage machinery made protocol-aware.

**Architecture:** Generalize four existing units rather than duplicate: a count-driven fob nav, a protocol-aware fob view (KeeLoq 4-tile / VAG 3-tile), a VAG branch in the fob scene (build no-desync state from the loaded `.sub`, TX by re-serializing `Cnt`/`Btn` so the VAG encoder re-encrypts), and a widened Save gate in the rolling analyzer that serializes a decoded VAG straight to the Garage.

**Tech Stack:** C, Flipper GUI (View/ViewModel, scene manager), ESP-IDF/ninja, host C test for pure logic.

## Global Constraints

- Private-only. No `#ifndef PUBLIC_BUILD` guard added here (tracked debt for the whole clone/Car-Keys surface); never ship on a public build/webflasher.
- KeeLoq behavior must remain byte-for-byte unchanged — VAG is strictly additive.
- VAG button table: `0x1=Unlock, 0x2=Lock, 0x4=Boot` (matches `vag_button_name`). KeeLoq table unchanged: `0x1/0x2/0x4/0x8 = LOCK/UNLOCK/TRUNK/PANIC`.
- Counter step for VAG no-desync: first emulate `cnt = captured_cnt + jump` (jump reuses `subghz->keeloq_clone_jump`, default 100), then `+1` per press. Single frame per press (VAG encoder repeats it 10× internally).
- VAG protocol name: `VAG_PROTOCOL_NAME` from `lib/subghz/protocols/vag.h`.

---

### Task 1: Count-driven fob nav

**Files:**
- Modify: `applications/main/subghz/helpers/subghz_fob_nav.c`
- Modify: `applications/main/subghz/helpers/subghz_fob_nav.h`
- Test: `applications/main/subghz/helpers/test/subghz_fob_nav_test.c` (host)

**Interfaces:**
- Produces: `uint8_t subghz_fob_nav_move_n(uint8_t idx, uint8_t key, uint8_t n)` — `n==4` keeps the exact 2×2 spatial behavior of the current `subghz_fob_nav_move`; `n<4` is linear (LEFT/UP → prev, RIGHT/DOWN/CYCLE → next, mod n); result always `< n`. Existing `subghz_fob_nav_move(idx,key)` stays as a thin `_move_n(idx,key,4)` wrapper (KeeLoq callers unchanged).

- [ ] **Step 1: Write failing tests** for `_move_n`: `n=4` matches legacy 2×2 (LEFT toggles col, UP toggles row, CYCLE = (idx+1)&3); `n=3` CYCLE walks 0→1→2→0, RIGHT walks forward mod 3, LEFT walks 0→2→1→0, and any idx≥n clamps into range.
- [ ] **Step 2: Run tests, verify they fail** (symbol undefined).
- [ ] **Step 3: Implement** `_move_n`: for `n>=4` run the current row/col logic; for `n<4` linear step mod n; clamp `idx %= (n?n:1)` on entry. Make `subghz_fob_nav_move` call `_move_n(idx,key,4)`.
- [ ] **Step 4: Run tests, verify pass.**
- [ ] **Step 5: Commit** `feat(subghz): count-driven fob nav for N tiles`.

---

### Task 2: Protocol-aware fob view

**Files:**
- Modify: `applications/main/subghz/views/subghz_keeloq_fob.c`
- Modify: `applications/main/subghz/views/subghz_keeloq_fob.h`

**Interfaces:**
- Consumes: `subghz_fob_nav_move_n` (Task 1).
- Produces:
  - `typedef enum { SubGhzFobKindKeeloq = 0, SubGhzFobKindVag } SubGhzFobKind;` (in `.h`).
  - `subghz_keeloq_fob_set_state(instance, SubGhzFobKind kind, uint8_t real_btn, uint16_t cnt, uint16_t jump)` — **kind arg added** (callers in Task 3 updated).
  - `subghz_keeloq_fob_selected_nibble(instance)` returns the code from the **active** table.

- [ ] **Step 1:** Add `SubGhzFobKind` to the header; add `kind` arg to `subghz_keeloq_fob_set_state`.
- [ ] **Step 2:** In `.c` add a second table `VAG_CODES[3]={0x1,0x2,0x4}`, `VAG_LABELS[3]={"UNLOCK","LOCK","BOOT"}`; add `uint8_t kind; uint8_t n_tiles;` to the model. Add a static helper returning `(codes, labels, n)` for the model's kind.
- [ ] **Step 3:** Draw callback iterates `m->n_tiles` (not 4); use the active labels/codes; for VAG (3 tiles) render cells 0,1,2 of the existing `rx/ry` grid (top-left, top-right, bottom-left); footer for VAG appends a short "adv cnt-may desync" note (KeeLoq footer unchanged).
- [ ] **Step 4:** Input callback uses `subghz_fob_nav_move_n(m->sel, navkey, m->n_tiles)`.
- [ ] **Step 5:** `set_state` stores `kind`, sets `n_tiles` (4 KeeLoq / 3 VAG), maps `real_btn` to `sel` over the active table; `selected_nibble` reads the active table; alloc default kind = KeeLoq (`n_tiles=4`) so nothing regresses before `set_state`.
- [ ] **Step 6: Build check** (compile the view TU) — deferred to Task 5 full build; ensure no obvious type errors by review.
- [ ] **Step 7: Commit** `feat(subghz): protocol-aware fob view (KeeLoq 4-tile / VAG 3-tile)`.

---

### Task 3: Fob scene VAG branch

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c`

**Interfaces:**
- Consumes: protocol-aware `subghz_keeloq_fob_set_state` (Task 2); VAG encoder via `subghz_tx_start` re-reading `Cnt`/`Btn` from `fff_data`.

- [ ] **Step 1:** On enter, after resolving `from_garage`, read `Protocol` from `fff_data`. If it equals `VAG_PROTOCOL_NAME`, take the VAG path; else the existing KeeLoq path (unchanged).
- [ ] **Step 2: VAG enter:** read `Serial/Btn/Cnt/KeyIdx/Type` from `fff`; compute `cnt_start = (uint16_t)(cnt + (jump?jump:100))` on a fresh clone, or the saved `Cnt` when `from_garage`; write `cnt_start` back into `fff` ("Cnt"); `subghz_keeloq_fob_set_state(fob, SubGhzFobKindVag, (uint8_t)(btn & 0x7), cnt_start, jump)`.
- [ ] **Step 3: VAG press:** in `on_event` Send, when kind is VAG (track via a `subghz->` scene flag or re-read Protocol), set `btn` = selected nibble and `cnt` = current+1, `flipper_format_update_uint32(fff,"Btn",&btn,1)` + `..."Cnt",&cnt`, then `subghz_tx_start(subghz, fff)` (VAG encoder re-encrypts). Reuse the existing TX-complete tick + `subghz_keeloq_fob_set_sent` refresh.
- [ ] **Step 4:** Ensure KeeLoq calls to `set_state` pass `SubGhzFobKindKeeloq` (signature change from Task 2).
- [ ] **Step 5: Commit** `feat(subghz): VAG no-desync branch in fob scene`.

---

### Task 4: Rolling-analyzer VAG save gate + branch

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c`

**Interfaces:**
- Consumes: VAG decoder `.serialize`; `subghz_save_protocol_to_file`; fob scene VAG path (Task 3).

- [ ] **Step 1:** Add `subghz_scene_rolling_analyzer_can_save_vag(protocol_name, decoded_text)`: true when `protocol_name==VAG_PROTOCOL_NAME` and `decoded_text` contains `"Ser:"` and does NOT contain `"corrupted"`. Keep `can_clone` (KeeLoq) exactly as-is.
- [ ] **Step 2:** In `on_enter`, show the Save/Clone buttons when `can_clone(...) || can_save_vag(...)`. For VAG show **Save** only (Clone == Save for VAG; no separate rebuild path) — a single `GuiButtonTypeRight "Save"`.
- [ ] **Step 3:** In `on_event`, after the fresh reload/deserialize + get_string, branch: if `can_clone` → existing KeeLoq path (unchanged). Else if `can_save_vag` → serialize the decoder to a fresh `FlipperFormat` (`subghz_protocol_decoder_base_serialize(decoder, fff_out)`), derive make from VAG type byte + serial (parse `"Ser:"`), save to `"%s/garage/%s_%08lX.sub"`, set `subghz->keeloq_fob_from_garage = true`, `scene_manager_next_scene(..., SubGhzSceneKeeloqFob)`.
- [ ] **Step 4:** Confirm no KeeLoq regression: the KeeLoq gate/branch bytes are untouched; VAG is an added `else if`.
- [ ] **Step 5: Commit** `feat(subghz): Save decoded VAG to Garage + open fob`.

---

### Task 5: Build both targets, fix, host tests green

**Files:**
- Build: `src_m5stick_s3` (M5Stick target + Cardputer target).

- [ ] **Step 1:** Run the host test for Task 1 nav; green.
- [ ] **Step 2:** Full ninja build of the M5Stick target; fix any errors (types, includes, signature ripples).
- [ ] **Step 3:** Build the Cardputer target; fix.
- [ ] **Step 4:** Adversarial review (workflow) of the whole diff; address confirmed findings.
- [ ] **Step 5:** Flash M5Stick (by MAC) + rebuild Cardputer bin. **Commit** any fixups.

## Self-Review

- **Spec coverage:** Save gate (Task 4), protocol-aware view (Task 2), fob VAG branch (Task 3), garage routing (existing, verified in Task 3 enter), counter mechanic (Task 3 step 2/3), risks/warning footer (Task 2 step 3). Covered.
- **Placeholder scan:** none — the only deferred detail (make-name mapping specifics) is fully specified (VAG type byte → VW/Audi/Seat/Skoda/VAG, from `vag.c:1490`).
- **Type consistency:** `subghz_keeloq_fob_set_state` gains `SubGhzFobKind kind` in Task 2 and every caller (Task 3) is updated in the same branch; `subghz_fob_nav_move_n` signature is consistent across Tasks 1–2.
