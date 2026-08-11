# P2a: Fast Framed Sweep — Implementation Plan

> Implement task-by-task. Private-default via `#ifdef`; public behavior unchanged.

**Goal:** Make a SubBrute framed sweep ~2× faster (repeat clamp + configure-once + trim dead time) behind a `fast_mode` flag with a private toggle; public path untouched.

**Architecture:** A `fast_mode` session flag on `SubBruteWorker` gates three timing changes in `subbrute_worker.c`; a private-only toggle in the Bf extra-settings scene sets it. Default true (private) / false (public).

## Global Constraints

- `fast_mode` default: `#ifndef PUBLIC_BUILD` true `#else` false — one guard at worker alloc. Toggle UI `#ifndef PUBLIC_BUILD`. Public never enables fast mode → identical behavior.
- Clamp repeats to **2** floor (`SUBBRUTE_FAST_REPEAT`); never touch the frame guard gap.
- Build one env at a time. Private: `build_m5stick_s3.bat`. Public gate: `build_m5stick_s3_public.bat`.
- nm: `C:\Users\stone\.espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe`. No local gcc.

---

### Task 1: fast_mode flag + repeat clamp + delay trims (low-risk core)

**Files:** Modify `helpers/subbrute_worker_private.h`, `helpers/subbrute_worker.c`, `helpers/subbrute_worker.h`

**Interfaces produced:** `subbrute_worker_set_fast(SubBruteWorker*, bool)`, `bool subbrute_worker_is_fast(SubBruteWorker*)`, worker fields `fast_mode`, `radio_ready`.

- [ ] **Step 1 — Struct.** In `subbrute_worker_private.h` `struct SubBruteWorker`, add `bool fast_mode;` and `bool radio_ready;`.

- [ ] **Step 2 — Default + define.** In `subbrute_worker.c`: add `#define SUBBRUTE_FAST_REPEAT 2` near the file's other defines. In `subbrute_worker_alloc` set:
```c
#ifndef PUBLIC_BUILD
    instance->fast_mode = true;
#else
    instance->fast_mode = false;
#endif
    instance->radio_ready = false;
```

- [ ] **Step 3 — Repeat clamp.** In `subbrute_worker_thread`, just before the `if(instance->attack == SubBruteAttackLoadFile)` payload branch, compute:
```c
        uint8_t rep = (instance->fast_mode && instance->repeat > SUBBRUTE_FAST_REPEAT)
                          ? SUBBRUTE_FAST_REPEAT : instance->repeat;
```
and pass `rep` in place of `instance->repeat` in BOTH `subbrute_protocol_file_payload(...)` and `subbrute_protocol_default_payload(...)`.

- [ ] **Step 4 — Trim delays.** In `subbrute_worker_thread`, change the bottom-of-loop `furi_delay_ms(instance->tx_timeout_ms);` to `furi_delay_ms(instance->fast_mode ? 1 : instance->tx_timeout_ms);`. In `subbrute_worker_subghz_transmit`, add `const uint8_t poll_ms = instance->fast_mode ? 1 : instance->tx_timeout_ms;` and use `poll_ms` at the two `furi_delay_ms(timeout)` poll waits (the `while(instance->transmit_mode)` guard and the `while(!...is_async_complete_tx...)` wait).

- [ ] **Step 5 — Accessors.** Add `subbrute_worker_set_fast` (sets `instance->fast_mode`) and `subbrute_worker_is_fast` (returns it) in `subbrute_worker.c`; declare both in `subbrute_worker.h`.

- [ ] **Step 6 — Build + verify.** `build_m5stick_s3.bat` → `Project build complete`; `nm build_m5stick_s3/furi_esp32.elf | grep subbrute_worker_set_fast` → PRESENT.

- [ ] **Step 7 — Commit.** `feat(subghz): SubBrute fast-mode flag + repeat clamp + trimmed delays (private default)`

---

### Task 2: Configure-radio-once (config hoist)

**Files:** Modify `helpers/subbrute_worker.c`

**Interfaces consumed:** `fast_mode`, `radio_ready` (Task 1).

- [ ] **Step 1 — Gate the config block.** In `subbrute_worker_subghz_transmit`, wrap the four config calls (`subghz_devices_reset/idle/load_preset/set_frequency`, currently ~465-468):
```c
        if(!instance->fast_mode || !instance->radio_ready) {
            subghz_devices_reset(instance->radio_device);
            subghz_devices_idle(instance->radio_device);
            subghz_devices_load_preset(instance->radio_device, instance->preset, NULL);
            subghz_devices_set_frequency(instance->radio_device, instance->frequency);
            instance->radio_ready = true;
        }
```

- [ ] **Step 2 — Reset radio_ready per sweep.** In `subbrute_worker_thread`, set `instance->radio_ready = false;` immediately before the `while(instance->worker_running)` loop (so the first code configures once).

- [ ] **Step 3 — Manual TX safety.** In `subbrute_worker_transmit_current_key`, set `instance->radio_ready = false;` before it calls `subbrute_worker_subghz_transmit`, so single-shot manual TX always re-configures (freq/preset may have changed).

- [ ] **Step 4 — Build + verify.** `build_m5stick_s3.bat` → green. (On-device validation flagged: confirm a fast sweep still keys the radio correctly on every code — this is the medium-risk change.)

- [ ] **Step 5 — Commit.** `feat(subghz): configure radio once per fast sweep (skip per-code re-arm)`

---

### Task 3: Fast toggle UI (private-only)

**Files:** Modify `scenes/subghz_scene_bf_setup_extra.c`

**Interfaces consumed:** `subbrute_worker_set_fast` / `_is_fast` (Task 1); `subghz->subbrute_worker`.

- [ ] **Step 1 — Add the item.** Under `#ifndef PUBLIC_BUILD` in `subghz_scene_bf_setup_extra_on_enter`, add a "Fast" `VariableItem` with 2 values ("Off"/"On") via `variable_item_list_add`, initialise its current index from `subbrute_worker_is_fast(subghz->subbrute_worker) ? 1 : 0`, and set its change callback.

- [ ] **Step 2 — Change callback.** Under `#ifndef PUBLIC_BUILD`, in the item's change callback call `subbrute_worker_set_fast(subghz->subbrute_worker, variable_item_get_current_value_index(item) == 1)` and set the value text ("Off"/"On"). Follow the existing item pattern in this scene.

- [ ] **Step 3 — Build + verify.** `build_m5stick_s3.bat` → green; `grep -a` the private app bin for the "Fast" item label → present.

- [ ] **Step 4 — Commit.** `feat(subghz): Fast-sweep On/Off toggle in Bf extra-settings (private-only)`

---

### Task 4: Public-behavior gate

**Files:** none (verification).

- [ ] **Step 1 — Build public.** `build_m5stick_s3_public.bat` → SENTINEL_OK.
- [ ] **Step 2 — Default gate.** Confirm the `#ifdef` sets `fast_mode=false` in public (inspect the compiled path / the source guard); public sweep uses full repeats + per-code config.
- [ ] **Step 3 — String gate.** `grep -a` the public merged bin for the "Fast" toggle label → **0** (the toggle UI is private-only).
- [ ] **Step 4 — Car gate.** `python tools/verify_public_no_car.py Flipper-m5stick_s3-public-merged.bin` → PASS.
- [ ] **Step 5 — Commit.** `chore(subghz): verify fast-sweep is private-default + public unchanged`

---

## Self-review

- **Coverage:** Task 1 = repeat clamp + delays + flag (spec §Design 1,3), Task 2 = configure-once (spec §Design 2), Task 3 = toggle (spec §Fast toggle), Task 4 = public gate (spec §Global constraint 1). All mapped.
- **Type consistency:** `fast_mode`/`radio_ready`/`SUBBRUTE_FAST_REPEAT`/`subbrute_worker_set_fast`/`_is_fast` used identically across tasks.
- **No placeholders:** exact call sites + code given; the one write-time lookup is the exact `VariableItemList` add-item call in the Bf extra scene (follow the existing items there).
