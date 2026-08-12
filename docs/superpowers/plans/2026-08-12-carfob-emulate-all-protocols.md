# Extend car-fob multi-button emulate to all protocols — implementation plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** every car protocol in `lib/subghz/protocols/` opens the multi-button rolling fob view
(`views/subghz_keeloq_fob`), synthesizing each button via its own encoder with counter
advance+persist. Generalize the existing view; VAG/KeeLoq keep working unchanged.

**Spec:** `docs/superpowers/specs/2026-08-12-carfob-emulate-all-protocols-design.md`.

## Global Constraints
- App = `applications/main/subghz` + `lib/subghz/protocols` ONLY. Do NOT touch proto_pirate.
- No regression to the existing VAG (`SubGhzFobKindVag`) / KeeLoq (`SubGhzFobKindKeeloq`) emulate.
- No new radio/TX stack; TX via the standard txrx path (protocol's own encoder), reusing the
  existing counter-jump + persist (`cnt_start = cnt + jump`, persist in subghz_tx_start).
- Builds via the PowerShell tool only; clean-reconfigure (rmdir build dir) when files added.
  proto_pirate stays excluded. Host tests via WSL gcc (run ELF inside wsl).

---
### Task 1: Audit + generic button-table provider
**Files:** create `applications/main/subghz/helpers/subghz_fob_buttons.{c,h}`, `tests/test_subghz_fob_buttons.c`.
**Produces:** `SubGhzFobButton{uint8_t code;const char* name;}`, `subghz_fob_buttons(name,&n)`,
`subghz_fob_protocol_supported(name)`.
- [ ] Audit `lib/subghz/protocols/*.c`: which car protocols have (a) an encoder whose deserialize
  reads an independent button field, and (b) an enumerable button set (button-name map). Record a
  synth/replay/unsupported table as a comment block. Reuse each protocol's existing name map.
- [ ] Build per-protocol `{code,name}[]` tables incl. the existing VAG (3-tile) + KeeLoq (4-tile)
  layouts. Dispatch by the protocol name string each protocol uses (`*_PROTOCOL_NAME`).
- [ ] Host test: VAG→3 buttons, KeeLoq→4, a Honda/Kia/Ford→their buttons, unknown→NULL. WSL gcc.
- [ ] Commit + report `docs/superpowers/plans/carfob-task1-report.md`.

### Task 2: Generalize the view
**Files:** modify `applications/main/subghz/views/subghz_keeloq_fob.{c,h}`.
- [ ] Add `subghz_keeloq_fob_set_table(instance, const SubGhzFobButton* buttons, size_t n, uint8_t real_btn, uint32_t cnt, uint16_t jump)`. Refactor the two hardcoded `SubGhzFobKind` layouts to pull from `subghz_fob_buttons` so render+nav is ONE path; keep `set_state(kind,...)` as a thin wrapper for the VAG/KeeLoq callers (no behavior change). `subghz_fob_nav` handles N tiles.
- [ ] Build private cardputer green; confirm VAG/KeeLoq screens render identically.
- [ ] Commit + report.

### Task 3: Route all supported protocols into the view + synth TX
**Files:** modify `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c` and the scene(s) that reach it (grep the routing / `keeloq_fob_from_garage`, receiver_info, saved).
- [ ] Add a generic branch: if `subghz_fob_protocol_supported(proto)` and not VAG/KeeLoq, read
  Protocol/Serial/Btn/Cnt from fff, call `set_table` with `subghz_fob_buttons(proto,...)`.
- [ ] On press: write `Btn`+next `Cnt` into fff (`flipper_format_update_uint32`), TX via the
  standard txrx path (the protocol's encoder), advance+persist counter (reuse existing jump/persist),
  refresh the on-screen counter. Guard: no advance on failed TX. VAG/KeeLoq branches unchanged.
- [ ] Ensure a captured/saved fob of a supported protocol actually reaches this scene (add routing
  from receiver_info/saved "Emulate"/fob entry if needed).
- [ ] Build private green. Commit + report.

### Task 4: Round-trip sweep + no-regression + build verification
**Files:** `tools/decoder_host_test/` (reuse), `tests/`.
- [ ] Host sweep: for each supported protocol × each button, synth (serial,cnt,btn) via the real
  lib/subghz encoder → decode → assert btn matches + counter advances. PASS/FAIL matrix, reasons for
  any expected non-pass. Reuse `keystore_host.c` for manufacturer keys.
- [ ] Build private m5stick + cardputer green (PowerShell, clean-reconfigure). Manually confirm
  VAG + KeeLoq emulate unchanged (no regression). Public build unaffected.
- [ ] Commit + report. Final whole-branch review.
