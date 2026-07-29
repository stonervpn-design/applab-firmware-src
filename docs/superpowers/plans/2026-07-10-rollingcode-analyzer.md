# Rolling-Code Analyzer + KeeLoq Decode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A read-only Rolling-Code Analyzer view for captured dynamic Sub-GHz signals (structured serial/counter/button/manufacturer + verdict), reachable from the receiver-info screen; plus enabling/documenting the plaintext KeeLoq manufacturer-key path for wider decode.

**Architecture:** Mirror the EXISTING TPMS specialized-view pattern in `subghz_scene_receiver_info.c` (`subghz_scene_receiver_info_try_tpms` switches to a TPMS view for TPMS protocols). Add an analogous path for `SubGhzProtocolTypeDynamic` that offers an "Analyze" action opening a new `rolling_analyzer` scene, which reads the already-decoded `SubGhzBlockGeneric` fields. Part B is keystore-path verification + docs (no fabricated keys, no AES).

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper subghz app (scenes, widget, protocol lib). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Reference pattern (READ FIRST):** `applications/main/subghz/scenes/subghz_scene_receiver_info.c` — the TPMS path: `subghz_scene_receiver_info_try_tpms` (~line 131), `subghz_scene_receiver_info_tpms_*` callbacks, `TPMSBlockGeneric generic` + `tpms_block_generic_deserialize`, and how it switches to the TPMS view. The analyzer mirrors this whole structure for dynamic protocols.
- **Read-only:** the analyzer must not transmit, re-decode, or mutate the capture. It only reads decoded fields and renders them. Counter-advance TX already exists (`subghz_scene_transmitter.c`) — do not touch it.
- **`SubGhzBlockGeneric`** (`lib/subghz/blocks/generic.h`): `serial` (u32), `cnt` (u32), `btn` (u8), `data_count_bit` (u16), `data` (u64), `type`. Deserialize a saved capture into one via the generic block deserialize (find the exact fn name in `generic.h`/`generic.c` — analogous to `tpms_block_generic_deserialize`; likely `subghz_block_generic_deserialize`).
- **No AES / no fabricated keys** (Part B). `furi_hal_crypto_decrypt` is a stub; the AES `keeloq_mfcodes` is unusable — do not attempt to fix it. Do not invent manufacturer key values.
- Repo is git on `master`; commit per task. Leave untracked helper scripts alone.

---

### Task 1: Rolling-Code Analyzer scene + view + receiver-info hook

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c`
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register the scene)
- Modify: `applications/main/subghz/scenes/subghz_scene_receiver_info.c` (add the Analyze action for dynamic protocols + custom event)
- Modify: the subghz custom-events header (add the analyzer open event, mirroring `SubGhzCustomEventSceneReceiverInfoSave`)

**Interfaces:**
- Consumes: `SubGhzBlockGeneric` fields; `subghz_protocol_decoder_base_get_string`; the decoder's protocol `type`/`name`.
- Produces: the analyzer scene, reachable only for dynamic captures.

- [ ] **Step 1: Register the scene.** In `subghz_scene_config.h`, add after the receiver_info line:
  ```c
  ADD_SCENE(subghz, rolling_analyzer, RollingAnalyzer)
  ```

- [ ] **Step 2: Add the custom event.** In the subghz custom-event enum header (where `SubGhzCustomEventSceneReceiverInfoSave` is defined — grep it), add `SubGhzCustomEventSceneReceiverInfoAnalyze`.

- [ ] **Step 3: Receiver-info hook.** In `subghz_scene_receiver_info.c`:
  - Read how the scene gets the current decoder and its `protocol->type` (the TPMS path already does `decoder->protocol->type != SubGhzProtocolTypeTpms`). Add a helper `subghz_scene_receiver_info_is_dynamic(subghz)` returning `decoder->protocol->type == SubGhzProtocolTypeDynamic`.
  - In `subghz_scene_receiver_info_on_enter`, when the protocol is dynamic, add a **Left** button to the widget labeled "Analyze" (mirror how the existing Center/Right buttons are added — `widget_add_button_element(..., GuiButtonTypeLeft, "Analyze", subghz_scene_receiver_info_callback, subghz)`), and in `subghz_scene_receiver_info_callback` handle `GuiButtonTypeLeft` → `view_dispatcher_send_custom_event(..., SubGhzCustomEventSceneReceiverInfoAnalyze)`.
  - In `subghz_scene_receiver_info_on_event`, handle that event → `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollingAnalyzer)`.
  - If the widget has no free Left slot (confirm the layout), instead add the Analyze button in the free position the TPMS/normal layout leaves; keep Center=TX and Right=Save intact.

- [ ] **Step 4: The analyzer scene.** Create `subghz_scene_rolling_analyzer.c` modeled on the receiver-info scene's structure (it uses `subghz->widget`). On enter:
  - Get the current decoder (same accessor the info scene uses, e.g. `subghz_txrx_get_decoder(subghz->txrx)`).
  - Deserialize the capture into a `SubGhzBlockGeneric generic = {0}` (use the generic deserialize fn — confirm name in `generic.h`; the info scene's TPMS branch shows the deserialize+fff pattern to copy). This yields `serial`, `cnt`, `btn`, `data_count_bit`.
  - Get the protocol name (`subghz_history_get_protocol_name(...)` as the info scene does, or `decoder->protocol->name`).
  - Get the manufacturer + full decoded text via `subghz_protocol_decoder_base_get_string(decoder, text)` (same call at receiver_info.c:190). Use it to display the manufacturer line (parse the "Manufacture" line if present) and as the authoritative text.
  - Build the widget: title "Rolling Code"; text elements for Protocol, Manufacturer (or "key unknown"), Serial `0x%08lX`, Button `%d`, Counter `0x%04lX (%lu)`, Bits `%d`; and a **verdict** line:
    - if `cnt` decoded (manufacturer known / cnt non-zero for a decoded dynamic): "Rolling: raw replay rejected; TX re-advances counter."
    - else: "Rolling: mfr key unknown — counter not decodable. Add key to keeloq_mfcodes_user."
    (Determine "decoded vs not" from whether the get_string output contains a real manufacturer/counter vs. the decoder's not-found state — read keeloq.c's get_string to see what it emits when no key matches.)
  - Back returns to receiver-info (default scene-manager back).
  - `on_exit`: `widget_reset(subghz->widget)`.

- [ ] **Step 5: Build** → `build_rca1.log`, expect `=== BUILD OK ===`, EXIT=0. Fix any accessor-name mismatches by reading `generic.h` / `subghz_protocol_decoder_base.h` / `keeloq.c`.

- [ ] **Step 6: Commit**
```bash
git add applications/main/subghz/
git commit -m "feat(subghz): rolling-code analyzer view for dynamic captures"
```

---

### Task 2: KeeLoq decode enablement (plaintext keystore path + docs)

**Files:**
- Modify: `applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user.example` (docs)
- Read/verify: `lib/subghz/subghz_keystore.c` (plaintext load), `lib/subghz/protocols/keeloq.c` (key iteration), the environment keystore load path.

- [ ] **Step 1: Verify the plaintext path end-to-end.** Read `subghz_keystore_read_file` and `subghz_environment_load_keystore`: confirm a `keeloq_mfcodes_user` with `Encryption: 0` is loaded and its keys are added to the same keystore `keeloq.c` iterates. Confirm the (unusable) AES `keeloq_mfcodes` failing to load does NOT prevent the plaintext user file from loading (i.e. they're independent loads / the user file isn't gated behind the AES one). If the user file is only loaded conditionally on the AES file succeeding, fix the load order so the plaintext file always loads. Document findings in the report; make a code fix ONLY if the plaintext file is genuinely not being loaded.

- [ ] **Step 2: Improve the example docs.** In `keeloq_mfcodes_user.example`, keep the existing format header and add clear notes: the SD path to place the real file (`/ext/subghz/assets/keeloq_mfcodes_user`), that `Encryption: 0` (plaintext) is required on this port (AES keystores don't work — no secure enclave), and that a full public plaintext manufacturer-key list can be dropped there for broad KeeLoq coverage. Do NOT add fabricated keys.

- [ ] **Step 3: Seed only if public plaintext keys exist in-repo/upstream.** Check whether the cloned upstream (`<scratchpad>/upstream-116/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user`) or anywhere in THIS repo already contains a populated plaintext (`Encryption: 0`) KeeLoq key list. If yes, copy those existing public entries into our shipped `keeloq_mfcodes_user` (copy-only, existing public data). If no populated plaintext source exists, leave `keeloq_mfcodes_user` as-is (the path works; keys are a user drop-in) and note it in the report. Do NOT fabricate keys.

- [ ] **Step 4: Build** → `build_rca2.log` (docs/asset change; build proves nothing broke), expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 5: Commit**
```bash
git add applications/main/subghz/ lib/subghz/
git commit -m "feat(subghz): document + verify plaintext KeeLoq keystore path for wider decode"
```

---

### Task 3: KeeLoq "By Manufacturer" generator (encoder for each of the 68)

**Goal:** a new Add-Manually entry that lets you pick ANY manufacturer from the loaded plaintext keystore (the 68), then generate + transmit a KeeLoq signal — reusing the existing `GenKeeloq` serial/button/counter entry and `subghz_txrx_gen_keeloq_protocol()` encoder. No new encoder; a manufacturer picker + wiring.

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_keeloq_mfr.c` (manufacturer picker scene)
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register the scene)
- Modify: `applications/main/subghz/scenes/subghz_scene_set_type.c` (add the "KL: By Manufacturer" submenu entry + route it to the picker)

**Established facts (don't re-investigate):**
- `subghz_scene_set_type.c` drives generation: a `submenu_names[SetTypeMAX]` list; selection sets `subghz->gen_info` (`GenInfo`), and `subghz_scene_set_type_generate_protocol_from_infos` switches on `gen_info.type` — `case GenKeeloq:` calls `subghz_txrx_gen_keeloq_protocol(mod, freq, gen_info.keeloq.serial, .btn, .cnt, .manuf)`.
- The hardcoded KeeLoq entries (e.g. `SetTypeDoorHan_433_92`) each fix a manufacturer + frequency. We add ONE dynamic entry instead.
- Keystore iteration: `SubGhzKeystore* ks = subghz_environment_get_keystore(environment)`; `SubGhzKeyArray_t* arr = subghz_keystore_get_data(ks)`; iterate `M_EACH(k, *arr, SubGhzKeyArray_t)` → `furi_string_get_cstr(k->name)` is the manufacturer name (the 68). Get the environment the same way the subghz app already does (grep `subghz_txrx_get_environment` / how set_type reaches `subghz->txrx`'s environment).

- [ ] **Step 1: Read the GenKeeloq routing.** In `subghz_scene_set_type.c`, trace how selecting a hardcoded KeeLoq entry leads to: setting `gen_info.type=GenKeeloq`, `gen_info.keeloq.manuf`, `gen_info.mod`, `gen_info.freq`, then routing to the serial/button/counter **byte-input** entry scene, then to generation + the transmitter. Note the exact scene(s) used for the byte input and the state/enter-callback mechanism. Document it in your report. This is the flow you will re-enter after the manufacturer is picked.

- [ ] **Step 2: Register the picker scene.** Add `ADD_SCENE(subghz, keeloq_mfr, KeeloqMfr)` to `subghz_scene_config.h`.

- [ ] **Step 3: Add the Add-Manually entry.** In `subghz_scene_set_type.c`, add a new enum value `SetTypeKeeloqByMfr` to the `SetType` enum and `submenu_names[SetTypeKeeloqByMfr] = "KL: By Manufacturer"`. In the submenu selection handler, when `SetTypeKeeloqByMfr` is chosen, instead of populating a fixed manufacturer, set the defaults it shares (`gen_info.type = GenKeeloq`, `gen_info.mod` = AM650/AM270 per the other KL entries, `gen_info.freq` = 433920000 — match how a sibling KL 433MHz entry sets mod/freq) and `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqMfr)` (the picker) — do NOT jump straight to byte input.

- [ ] **Step 4: The manufacturer picker scene** `subghz_scene_keeloq_mfr.c` (model on any submenu-based subghz scene, e.g. how `set_type` builds its submenu):
  - `on_enter`: build `subghz->submenu` (reset first); iterate the keystore (`subghz_keystore_get_data`) and `submenu_add_item(subghz->submenu, furi_string_get_cstr(k->name), index, callback, subghz)` for each manufacturer. Keep an index→name mapping (store the picked name into `gen_info.keeloq.manuf` in the callback — copy the string into the gen_info's manuf buffer; check the `manuf` field type in `GenInfo` (a fixed char buffer vs const char*) and copy accordingly with bounds). Switch to the submenu view.
  - callback (item selected): set `subghz->gen_info->keeloq.manuf` to the chosen manufacturer name, then route into the EXISTING GenKeeloq serial/button/counter byte-input flow identified in Step 1 (call the same `scene_manager_next_scene(...)` that a hardcoded GenKeeloq entry uses to reach byte input). From there the existing flow handles serial/btn/counter → generate (`subghz_scene_set_type_generate_protocol_from_infos` already reads `gen_info.keeloq.manuf`) → transmitter.
  - `on_exit`: `submenu_reset(subghz->submenu)`.
  - If the keystore is empty (no plaintext keys loaded), show a message ("No manufacturer keys — add keeloq_mfcodes_user") instead of an empty submenu.

- [ ] **Step 5: Build** → `build_rca3.log`, expect `=== BUILD OK ===`, EXIT=0. Fix by reading `set_type.c` / `GenInfo` (`gen_info.h`) / keystore API for exact names. Do NOT change the encoder or `subghz_txrx_gen_keeloq_protocol`.

- [ ] **Step 6: Commit**
```bash
git add applications/main/subghz/
git commit -m "feat(subghz): KeeLoq generator by keystore manufacturer (all 68)"
```

---

### Task 4: Docs — on-hardware test checklist + README note

**Files:**
- Create: `docs/ROLLING_ANALYZER_TEST.md`
- Modify: `README.md`

- [ ] **Step 1: Test checklist** `docs/ROLLING_ANALYZER_TEST.md`, covering ALL THREE parts:
  - **Analyzer:** capture a static fob → receiver-info shows NO Analyze button; capture a dynamic fob (KeeLoq garage remote) → Analyze (Left) appears → shows Protocol/Manufacturer/Serial/Button/Counter/Bits + verdict; with a matching plaintext key the counter/manufacturer decode, without it "key unknown"; TX/Save from receiver-info still work.
  - **Keystore:** the 68 plaintext manufacturer keys load; a fob from a listed manufacturer now decodes its counter.
  - **Generator:** Add Manually → "KL: By Manufacturer" → the picker lists all 68 keystore manufacturers → pick one → enter serial/button/counter → transmit a valid KeeLoq signal (verify a matching receiver responds, or capture-back and confirm it decodes to the chosen manufacturer/serial). Empty keystore shows the "no keys" message.
  - Mark "not yet hardware-validated — requires physical Cardputer ADV + a dynamic-protocol remote/receiver."
- [ ] **Step 2: README note** under Sub-GHz: rolling-code analyzer (structured serial/counter/button/manufacturer + verdict for dynamic captures); plaintext KeeLoq keystore (drop keys into `keeloq_mfcodes_user`, ~68 shipped); and the KeeLoq "By Manufacturer" generator (Add Manually → pick any keystore manufacturer → serial/button/counter → transmit).
- [ ] **Step 3: Commit**
```bash
git add docs/ROLLING_ANALYZER_TEST.md README.md
git commit -m "docs: rolling-code analyzer test checklist + README note"
```

---

## Self-Review

**Spec coverage:** Part A analyzer view → Task 1 (mirrors TPMS pattern, reads SubGhzBlockGeneric); receiver-info hook for dynamic only → Task 1 Step 3; Part B plaintext keystore enablement + docs, no AES/no fabricated keys → Task 2; test/README → Task 3. ✅

**Placeholder scan:** the "confirm the generic-deserialize fn name / get_string not-found output" instructions are concrete read-then-adapt against named files (the TPMS branch is the working template), not vague TODOs. Part B Step 3 explicitly does copy-only-if-present, no fabrication.

**Type consistency:** `SubGhzCustomEventSceneReceiverInfoAnalyze` (Task 1 Step 2) used in Step 3; `SubGhzSceneRollingAnalyzer` from the ADD_SCENE macro (Step 1) used in Step 3; `SubGhzBlockGeneric` fields consumed in Step 4.

**Ordering:** Task 1 (analyzer) and Task 2 (keystore) are largely independent; Task 1 first (the analyzer displays what Task 2 enables). Task 3 last.

**Honesty note to carry to the user:** Part B's actual manufacturer keys are a user-supplied/public data drop-in — the engineering delivers the working path + docs, not fabricated keys. If upstream/repo has a public plaintext list, it's seeded; otherwise the file stays minimal with working infrastructure.
