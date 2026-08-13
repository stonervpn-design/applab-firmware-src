# CAN-Bus Fuzzer (CAN1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add a CAN-bus fuzzer to the CAN Commander app: mutate a base `CanFrame` (ID/DLC/data/flags) and transmit on the TWAI bus in a rate-limited, abortable loop, surfacing the real TWAI bus-state reaction.

**Architecture:** A pure host-tested mutation core (`can_fuzz`) mutates the existing furi-free `CanFrame`; a new scene (cloning `scene_periodic.c`'s FuriThread worker/abort/status) drives `can_fuzz_next → can_hal_transmit` and reads `can_hal_stats()` for an honest bus-reaction indicator. Spec: `docs/superpowers/specs/2026-08-13-can-fuzzer-design.md`.

**Tech Stack:** C (ESP-IDF TWAI/furi), FuriThread worker, WSL gcc host tests. Branch `feat/redteam-gapfill` (continues after WF1).

## Global Constraints
- **PRIVATE-only — inherited via manifest, no in-source guard.** CAN Commander is removed from the public `APPS` list in `fam_config.py:272-274` when `_is_public`. Do NOT add `#ifndef PUBLIC_BUILD` to the new files (the app uses none). Task 3 PROVES the whole app + fuzzer are absent from the public ELF (`can_fuzz`/`can_commander` symbols = 0).
- **No new TX/driver path.** TX only via `can_hal_transmit(&frame, timeout)` (`can_hal.h`); lifecycle via `can_hal_start(cfg)`/`can_hal_stop()`/`can_hal_is_running()`; stats via `can_hal_stats()` (`CanHalStats {tx_ok,rx_ok,tx_fail,bus_err,bus_off}`); `can_hal_recover()` on bus-off.
- **Every mutated frame must be TWAI-valid:** `dlc ∈ [0,8]`; `id` masked to `CAN_ID_STD_MAX`(0x7FF) or `CAN_ID_EXT_MAX`(0x1FFFFFFF) per `extended`. Never emit a frame `can_hal_transmit` would reject.
- **Honest UI:** show real `can_hal_stats()` counters; state "bus state = real TWAI counters (no-ACK may mean no device), not a crash verdict."
- **Fresh-log build method** (delete build dir → absolute-path `.bat` → verify fresh log mtime + SENTINEL + obj recompiled). Private: `build_m5stick_s3.bat`, `build_adv_only_private.bat`. Public: `build_m5stick_s3_public.bat`.

---

### Task 1: Mutation core `can_fuzz.{c,h}` + host test

**Files:**
- Create: `applications/main/can_commander/helpers/can_fuzz.h`
- Create: `applications/main/can_commander/helpers/can_fuzz.c`
- Test: `tests/test_can_fuzz.c`
- (Read: `applications/main/can_commander/can_frame.h` for the exact `CanFrame` struct + `CAN_MAX_DATA`/`CAN_ID_STD_MAX`/`CAN_ID_EXT_MAX`.)

**Interfaces:**
- Produces: `typedef enum { CFZ_DATA_BITFLIP, CFZ_DATA_RANDOM, CFZ_ID_SWEEP, CFZ_DLC_SWEEP, CFZ_FULL_RANDOM } CanFuzzMode;` (`CFZ_MODE_COUNT`=5); `void can_fuzz_next(const CanFrame* base, CanFuzzMode mode, uint32_t seed, uint32_t step, CanFrame* out);` If `base` or `out` is NULL → no-op. Always writes a TWAI-valid frame to `out`.
- Consumes: `CanFrame` from `can_frame.h` (furi/IDF-free). `can_fuzz.h` includes `can_frame.h`.

- [ ] **Step 1: Write the failing test** `tests/test_can_fuzz.c`:
```c
#include "can_fuzz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static CanFrame mk_base(void) {
    CanFrame b; memset(&b, 0, sizeof(b));
    b.id = 0x123; b.extended = false; b.rtr = false; b.dlc = 8;
    for(int i = 0; i < 8; i++) b.data[i] = (uint8_t)(i + 1);
    return b;
}

static int popcount8x(const uint8_t* a, const uint8_t* b, int n) {
    int d = 0;
    for(int i = 0; i < n; i++) { uint8_t x = a[i]^b[i]; while(x){d+=x&1;x>>=1;} }
    return d;
}

static void test_valid_and_deterministic(void) {
    CanFrame b = mk_base();
    for(int m = 0; m < CFZ_MODE_COUNT; m++) {
        CanFrame x, y;
        can_fuzz_next(&b, (CanFuzzMode)m, 0xABCD, 11, &x);
        can_fuzz_next(&b, (CanFuzzMode)m, 0xABCD, 11, &y);
        assert(memcmp(&x, &y, sizeof(CanFrame)) == 0);       /* deterministic */
        assert(x.dlc <= 8);                                   /* TWAI-valid dlc */
        uint32_t max = x.extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX;
        assert(x.id <= max);                                  /* TWAI-valid id */
    }
    printf("test_valid_and_deterministic OK\n");
}

static void test_bitflip_one_data_bit(void) {
    CanFrame b = mk_base(), x;
    can_fuzz_next(&b, CFZ_DATA_BITFLIP, 0x7, 3, &x);
    assert(x.id == b.id && x.dlc == b.dlc && x.extended == b.extended);
    assert(popcount8x(x.data, b.data, b.dlc) == 1);          /* exactly one data bit */
    printf("test_bitflip_one_data_bit OK\n");
}

static void test_id_sweep_bounds(void) {
    CanFrame b = mk_base(); /* std */
    for(uint32_t s = 0; s < 5000; s++) {
        CanFrame x; can_fuzz_next(&b, CFZ_ID_SWEEP, 0, s, &x);
        assert(x.id <= CAN_ID_STD_MAX);
    }
    CanFrame be = mk_base(); be.extended = true;
    for(uint32_t s = 0; s < 5000; s++) {
        CanFrame x; can_fuzz_next(&be, CFZ_ID_SWEEP, 0, s, &x);
        assert(x.id <= CAN_ID_EXT_MAX);
    }
    printf("test_id_sweep_bounds OK\n");
}

static void test_dlc_sweep_cycle(void) {
    CanFrame b = mk_base();
    for(uint32_t s = 0; s <= 8; s++) {
        CanFrame x; can_fuzz_next(&b, CFZ_DLC_SWEEP, 0, s, &x);
        assert(x.dlc == (s % 9));
    }
    printf("test_dlc_sweep_cycle OK\n");
}

static void test_null_guards(void) {
    CanFrame b = mk_base(), x;
    can_fuzz_next(NULL, CFZ_DATA_RANDOM, 0, 0, &x); /* no crash */
    can_fuzz_next(&b, CFZ_DATA_RANDOM, 0, 0, NULL); /* no crash */
    printf("test_null_guards OK\n");
}

int main(void) {
    test_valid_and_deterministic();
    test_bitflip_one_data_bit();
    test_id_sweep_bounds();
    test_dlc_sweep_cycle();
    test_null_guards();
    printf("test_can_fuzz: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails.** Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Wall -Wextra -Iapplications/main/can_commander tests/test_can_fuzz.c applications/main/can_commander/helpers/can_fuzz.c -o /tmp/tcf && /tmp/tcf'` Expected: FAIL (no file).

- [ ] **Step 3: Write `can_fuzz.h`** — include guard, `#include "../can_frame.h"`, the `CanFuzzMode` enum + `CFZ_MODE_COUNT`, doc comment, the `can_fuzz_next` prototype. (Path: helpers/ includes ../can_frame.h.) furi/IDF-free.

- [ ] **Step 4: Write `can_fuzz.c`** — `#include "can_fuzz.h"` + `<string.h>`. NO `#ifndef PUBLIC_BUILD` (whole-app manifest exclusion). `static uint32_t sm32(uint32_t* s)` splitmix32 (copy idiom from `applications/main/nrf24/helpers/nrf24_fuzz.c`). In `can_fuzz_next`: `if(!base||!out) return;` `*out = *base;` (carry id/flags/dlc/data); `uint32_t st = seed ^ (step*0x9E3779B9u);` then per mode:
  - `CFZ_DATA_BITFLIP`: if `out->dlc>0`, `uint32_t bit = sm32(&st) % ((uint32_t)out->dlc*8u); out->data[bit/8] ^= 1u<<(bit%8);`
  - `CFZ_DATA_RANDOM`: `for(i=0;i<out->dlc;i++) out->data[i]=(uint8_t)sm32(&st);`
  - `CFZ_ID_SWEEP`: `uint32_t max = out->extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX; out->id = step % (max+1u);` (step is uint32; for ext, max+1 = 0x20000000, fits.)
  - `CFZ_DLC_SWEEP`: `out->dlc = (uint8_t)(step % 9u); for(i=0;i<out->dlc;i++) out->data[i]=(uint8_t)sm32(&st);`
  - `CFZ_FULL_RANDOM`: `uint32_t max = out->extended ? CAN_ID_EXT_MAX : CAN_ID_STD_MAX; out->id = sm32(&st) & max; out->dlc = (uint8_t)(sm32(&st)%9u); for(i=0;i<out->dlc;i++) out->data[i]=(uint8_t)sm32(&st);`
  - `default:` treat as `CFZ_FULL_RANDOM`.
  Finally clamp for safety: `if(out->dlc>8) out->dlc=8;` and `uint32_t m2 = out->extended?CAN_ID_EXT_MAX:CAN_ID_STD_MAX; out->id &= m2;` (ID_SWEEP already ≤max via modulo; the mask is harmless there and guards the random paths). Ensure `data` bytes above `dlc` are left as copied from base (don't matter for TX).

- [ ] **Step 5: Run test to verify it passes.** Expected: `test_can_fuzz: ALL PASS`, `-Wall -Wextra` clean.

- [ ] **Step 6: Commit**
```
git add applications/main/can_commander/helpers/can_fuzz.c applications/main/can_commander/helpers/can_fuzz.h tests/test_can_fuzz.c
git commit -m "can: CAN-frame fuzz mutators (5 modes, TWAI-valid) + host test (CAN1 Task 1)"
```

---

### Task 2: Fuzz scene + menu wiring + private build

**Files:**
- Create: `applications/main/can_commander/scenes/scene_fuzz.c`
- Modify: `applications/main/can_commander/scenes/can_commander_scene_config.h` (add `ADD_SCENE(can_commander, fuzz, Fuzz)`)
- Modify: `applications/main/can_commander/scenes/scene_main.c` (submenu item + dispatch)
- Modify: `applications/main/can_commander/can_commander_custom_event.h` (add `CanCommanderEvtFuzz`)
- (Read for pattern: `scenes/scene_periodic.c` (worker/abort/status), `can_hal.h`, `can_commander_app_i.h` (CanCommanderApp: `tx_frame`, `bitrate/mode/tx_gpio/rx_gpio`, `presets`), `can_frame.h`.)

**Interfaces:**
- Consumes: Task 1 `can_fuzz_next`/`CanFuzzMode`; `can_hal_transmit`/`can_hal_start`/`can_hal_stop`/`can_hal_is_running`/`can_hal_stats`/`can_hal_recover` (`can_hal.h`); `app->tx_frame`, session `CanHalConfig` fields (`can_commander_app_i.h`).
- Produces: scene id `CanCommanderSceneFuzz` (via ADD_SCENE X-macro).

- [ ] **Step 1: Fuzz scene** — clone `scene_periodic.c`: a `FuriThread* fuzz_thread` + `volatile bool fuzz_run` on the app (add fields to `CanCommanderApp` if needed, mirroring `tx_thread`/`tx_run`; if adding fields, that's a 5th file — acceptable, note it), worker `can_fuzz_worker` that loops `while(app->fuzz_run){ can_fuzz_next(&base, mode, seed, step++, &f); if(can_hal_transmit(&f,20)) app->fuzz_count++; stepped-sleep(interval, checking fuzz_run each ≤20ms); }`. Base = `app->tx_frame` if it has a nonzero dlc/id else a default (id 0x100, dlc 8, data 0..7). Mode via a VariableItem (5 modes). Start: `can_hal_start(cfg)` if `!can_hal_is_running()`. On Tick (200ms): read `can_hal_stats()`, render `Sent %lu  fail %lu  err %lu` + a `BUS-OFF` flag; if `bus_off` call `can_hal_recover()` once. Honest status line: "Bus state=real TWAI (no-ACK may = no device). Not a crash verdict." OK toggles start/stop; `on_exit` stops+joins the worker + `can_hal_stop()`. Follow periodic's exact stop helper (`fuzz_run=false; furi_thread_join; furi_thread_free; NULL`).
- [ ] **Step 2: Wiring** — `ADD_SCENE(can_commander, fuzz, Fuzz)` at the end of `can_commander_scene_config.h`; `CanCommanderEvtFuzz` in `can_commander_custom_event.h`; a `submenu_add_item(..., "Fuzzer", CanCommanderEvtFuzz, ...)` + a `scene_manager_next_scene(app->scene_manager, CanCommanderSceneFuzz)` dispatch in `scene_main.c`. NO `#ifndef PUBLIC_BUILD`.
- [ ] **Step 3: Private m5stick build (fresh-log).** Delete `build_m5stick_s3`; run `build_m5stick_s3.bat` absolute-path. Verify `SENTINEL_OK` + `scene_fuzz.c.obj` + `can_fuzz.c.obj` compiled (fresh timestamps). Fix errors + rebuild until green.
- [ ] **Step 4: Commit**
```
git add applications/main/can_commander/scenes/scene_fuzz.c applications/main/can_commander/scenes/can_commander_scene_config.h applications/main/can_commander/scenes/scene_main.c applications/main/can_commander/can_commander_custom_event.h [applications/main/can_commander/can_commander_app_i.h if fields added]
git commit -m "can: CAN Fuzzer scene + menu wiring + honest TWAI bus-state indicator (CAN1 Task 2)"
```

---

### Task 3: Full verification + public exclusion + report

**Files:**
- Create: `docs/superpowers/plans/can1-report.md`

- [ ] **Step 1: Private cardputer build (fresh-log).** Delete `build_cardputer_adv`; run `build_adv_only_private.bat`. Verify `SENTINEL_OK` + new objs compiled.
- [ ] **Step 2: Public exclusion build (fresh-log).** Delete `build_m5stick_s3_public`; run `build_m5stick_s3_public.bat`. Verify `SENTINEL_OK` + `NOCAR PASS`.
- [ ] **Step 3: nm/map exclusion scan.** WSL GNU `nm` on `build_m5stick_s3_public/furi_esp32.elf`: confirm total symbols large (tool works), then grep for `can_fuzz|can_commander|can_hal|CanFuzz` in ELF **and** `furi_esp32.map` → both **0** (proves the whole can_commander app, fuzzer included, is excluded from public via fam_config.py).
- [ ] **Step 4: Host test.** Re-run `test_can_fuzz` → ALL PASS.
- [ ] **Step 5: Report** `docs/superpowers/plans/can1-report.md` — what shipped, build results (3 targets, fresh mtimes), 0-public-symbol proof (whole-app manifest exclusion), host-test results, honest limits (transceiver required, DLC≤8, bus-state≠verdict), HW-deferred status.
- [ ] **Step 6: Commit**
```
git add docs/superpowers/plans/can1-report.md
git commit -m "can: CAN1 fuzzer verification report (CAN1 Task 3)"
```
- [ ] **Step 7: Final whole-branch review** of the CAN1 commits (frame-validity of every mutation, worker lifecycle/abort, honest bus-state indicator, public exclusion).

## Self-review notes
- **Spec coverage:** mutation core (T1), scene+wiring+private build (T2), verification incl. public exclusion + report (T3). All mapped.
- **Private-only:** inherited via fam_config.py APPS-exclusion; T3 proves 0 public symbols. No in-source guard by design.
- **Type consistency:** `CanFuzzMode`/`CFZ_*`/`can_fuzz_next` identical across T1/T2; `CanFrame`/`CanHalStats`/`can_hal_*` names match `can_frame.h`/`can_hal.h`.
- **Every mutated frame TWAI-valid** (dlc≤8, id masked) — enforced in the core + host-tested.
