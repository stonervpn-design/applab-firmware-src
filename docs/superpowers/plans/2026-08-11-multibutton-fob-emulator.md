# Multi-button rolling fob emulator — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development.
> Steps use `- [ ]` checkboxes.

**Goal:** Save a captured car fob as an identity fob and emulate it via a multi-button view
that synthesizes any button (serial+counter+button+keystore key), TXs, and advances+persists
the rolling counter. Replay-only protocols fall back to the captured button.

**Architecture:** Reuse the existing `protopirate_emulate_plugin` transmitter + counter fields
and the protocols' encoders. Add a button catalog, a button-grid view, and counter persistence.

**Tech Stack:** C, ESP-IDF FAM app (proto_pirate), lib/subghz encoders, flipper_format.
Build via PowerShell (`build_adv_only_private.bat` / `build_m5stick_s3.bat`); host tests via
WSL gcc reusing `tools/decoder_host_test/`.

## Global Constraints
- All new user-visible surface `#ifndef PUBLIC_BUILD`. Public build must stay car-free
  (`tools/verify_public_no_car.py`), nm shows 0 new symbols in public ELF.
- No new radio/TX stack. Manufacturer key comes from the keystore, never stored in the .sub.
- No-PSRAM Cardputer: no large allocs in the view; reuse existing buffers.
- Counter advances/persists ONLY on a successful TX.
- Run builds via the PowerShell tool, not Bash. Clean-reconfigure (rmdir build dir) when
  files are added/removed (CMake globs cached).

---

### Task 1: Button catalog + synth/replay audit

**Files:**
- Create: `applications_user/proto_pirate/protocols/protopirate_fob_buttons.c` / `.h`
- Test: `tests/test_fob_buttons.c`

**Interfaces — Produces:**
```c
typedef struct { uint32_t code; const char* name; } PPFobButton;
// Returns the protocol's button list (NULL,0 if none). name is a static string.
const PPFobButton* pp_fob_buttons(const char* protocol_name, size_t* out_n);
// True if the protocol's ENCODER can build a frame from (serial,counter,button) — i.e. its
// deserialize reads a button field AND it is not replay-only.
bool pp_fob_can_synthesize(const char* protocol_name);
```

- [ ] **Audit each TX-capable protocol.** For every `protocols/*.c` with an encoder, record:
  (a) does its encoder `deserialize` read a button field (`FF_BTN` directly or via
  `protocols_common.c` helper)? (b) does the protocol define/expose a button code→name set?
  Classify each as `synthesize` or `replay`. Write the findings as a comment block at the top
  of `protopirate_fob_buttons.c`.
- [ ] **Build the tables.** One `static const PPFobButton <proto>_buttons[]` per synth-capable
  protocol, reusing existing code maps (e.g. `vag_button_name` cases → {0x1,"Trunk"}, etc.;
  `honda_v1_button_codes`; kia button handling). `pp_fob_buttons` dispatches by canonical
  protocol name (reuse `protopirate_protocol_catalog` canonical-name lookup).
- [ ] **Write the failing test** `tests/test_fob_buttons.c`: assert VAG returns ≥4 buttons
  incl. a "Lock"/"Unlock"/"Trunk"/"Panic" name; a replay-only protocol returns
  `can_synthesize=false`; unknown name returns `(NULL,0)`. Build: `wsl gcc -I... tests/test_fob_buttons.c protocols/protopirate_fob_buttons.c -o /tmp/t && /tmp/t`.
- [ ] Implement minimal code to pass. Run. Commit.

### Task 2: Identity save carries Serial + Counter

**Files:**
- Modify: the save writer used by `scenes/protopirate_scene_need_saving.c` (find the actual
  serialize/write-key call) and any decoder `serialize` that omits `Serial`/`Cnt`.

**Interfaces — Consumes:** Task 1's `pp_fob_can_synthesize`.

- [ ] **Verify what save writes today** for a synth-capable protocol (VAG/Kia/Ford V0): does
  the saved `.sub` contain `Protocol`, `Frequency`, `Preset`, `Bit`, `Key`, `Serial`, `Cnt`?
  Use the host harness (`tools/decoder_host_test`) or read a decoder's `serialize`.
- [ ] For any synth-capable protocol whose `serialize` omits `Serial` or `Cnt`, add those
  writes (mirror an existing protocol that does). Do NOT store the manufacturer key beyond the
  existing `Key` field.
- [ ] Host test (extend `test_fob_buttons.c` or new): serialize a decoded VAG/Kia fob to an
  in-memory FlipperFormat, assert `Serial` and `Cnt` are present and re-readable. Run. Commit.

### Task 3: Multi-button emulate view

**Files:**
- Create: `applications_user/proto_pirate/views/protopirate_fob_emulate.c` / `.h`
- Modify: `views/protopirate_views.h` (register), view-id enum in the app types header.

**Interfaces — Produces:** a view that holds `{protocol_name, serial, counter, freq, preset,
buttons[], n, selected_idx, transmitting}` and a callback
`ProtoPirateCustomEventFobEmulateTx` fired with the selected button code on OK.

- [ ] Draw: header = protocol name (inverted while `transmitting`), `SN: CNT:%04lX`,
  `F:%s %s` (freq/preset), and the button grid (reuse `elements_button_*` or a simple 2-row
  layout matching the video). Navigation moves `selected_idx`; OK fires the TX callback; Back
  exits. `#ifndef PUBLIC_BUILD`.
- [ ] Wire alloc/free in `protopirate_app.c` behind `#ifndef PUBLIC_BUILD`. No host test
  (pure GUI); verify by build.
- [ ] Build private m5stick (PowerShell). Commit.

### Task 4: Synthesize + TX on button press

**Files:**
- Modify: `scenes/plugins/protopirate_emulate_plugin.c`, a new/updated scene routing loaded
  synth-capable fobs to the Task 3 view.

**Interfaces — Consumes:** Task 1 catalog, Task 3 view + TX event.

- [ ] On the TX event: build a FlipperFormat from `(protocol, serial, current_counter,
  selected button code)` and the fob's `.sub` fields; call the protocol's **encoder**
  `deserialize` (pulls key from keystore), then TX via the plugin's existing
  `SubGhzTransmitter` path (reuse `is_transmitting`, preset patch, MIN_TX_TIME_*).
- [ ] Replay-only protocols (`!pp_fob_can_synthesize`): deserialize the stored captured frame
  instead of synthesizing; the view shows only the captured button.
- [ ] Build private. On-device smoke deferred to owner. Commit.

### Task 5: Counter increment + persist-back

**Files:**
- Modify: `scenes/plugins/protopirate_emulate_plugin.c` (+ the fob `.sub` path).

- [ ] After a **successful** TX: `current_counter++`; update the view's counter; write the new
  `Cnt` back to the loaded fob `.sub` (open, update `Cnt`, save). Guard: no advance on
  aborted/failed TX. Mirror [[keeloq-no-desync-clone]] persistence.
- [ ] Host test: simulate N synth TXs, assert the persisted `Cnt` advanced by N and the next
  synthesized frame decodes with the advanced counter (reuse decoder_host_test round-trip).
  Run. Commit.

### Task 6: Per-protocol round-trip + build/gating verification

**Files:**
- Modify: `tools/decoder_host_test/pp_decode.c` (add a button-sweep mode), `tests/`.

- [ ] Add a harness mode: for each synth-capable protocol, for each catalog button, synthesize
  `(fixed serial, counter, button)` → decode → assert decoded btn == requested btn and counter
  matches. Run across the 5+ synth protocols; record pass/fail per (protocol,button).
- [ ] Build **private** m5stick + cardputer (PowerShell, clean-reconfigure — new files added).
  Build **public**; run `tools/verify_public_no_car.py`; `nm` the public ELF for 0 new fob
  symbols and the private ELF for their presence.
- [ ] Commit. Final whole-branch review (subagent-driven final step).
