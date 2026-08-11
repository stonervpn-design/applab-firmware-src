# BLE GATT Fuzzer + DoS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a private-only BLE tool — GATT fuzzer (mutation-sweep + MTU/long-write) plus a connection/write-flood DoS — layered on the existing BLE Walk explorer, target-address-locked, with a full target-info readout.

**Architecture:** Reuse `ble_spam`'s `ble_walk_hal` (scan/connect/discover/read/write). Promote the Sub-GHz fuzzer's `fuzzer_mutate` into a shared `components/fuzz_core` and reuse it for BLE payload mutation. New run engine + scenes + view in `ble_spam`, all `#ifndef PUBLIC_BUILD`. Open-loop with single-step + Mark-to-SD. Every mode binds to one explicitly-selected target (by BLE addr).

**Tech Stack:** C (ESP-IDF firmware), NimBLE central/GATT-client, Flipper-port GUI/scene system, host unit tests in C via WSL gcc.

## Global Constraints

- **Private-only inside a public app:** `ble_spam` ships on public. Guard EVERY new fuzz/DoS symbol `#ifndef PUBLIC_BUILD` (HAL additions, run engine, scenes, view, menu entries, scene enums). Existing BLE Walk stays public/untouched. Public build MUST show zero `ble_walk_fuzz_*` / `ble_walk_hal_dos_*` / `ble_walk_hal_fuzz_*` / `scene_walk_fuzz_*` / `scene_walk_target_info_*` symbols (nm); private has them.
- **Target-address lock:** all modes operate ONLY against the explicitly-selected `BleWalkDevice` (keyed on `addr`); the run engine refuses to arm with no target locked. Never a broadcast/any-device attack.
- **Authorized-testing only:** in-app note; short-range single-target; for devices the operator owns/is authorized to test.
- **No new radio stack:** reuse NimBLE via the existing `ble_walk_hal` connect/discover path; new writes use standard `ble_gattc_write_flat` / `ble_gattc_write_no_rsp_flat` / `ble_gattc_write_long`; MTU via `ble_att_set_preferred_mtu` + `ble_gattc_exchange_mtu`.
- **Builds:** a full firmware build (~18 min) exceeds the tool/subagent timeout — run full builds in the BACKGROUND, or use `idf.py -B build_m5stick_s3 [reconfigure] build` INCREMENTAL (cached objects, fast). Clean reconfigure (rm build dir) only when the component graph changes (Task 1) or scene_config changes. Build via the **PowerShell tool**, ESP-IDF v5.4.1 at `C:\Espressif\frameworks\esp-idf-v5.4.1`, `IDF_TOOLS_PATH=~/.espressif`. Never trust exit code alone — confirm `SENTINEL_OK` / "Project build complete" + fresh bin mtime.
- **Host tests:** `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra tests/<t>.c -o /tmp/<t> && /tmp/<t>"`; tests `#include` the module `.c` and assert in `main()`.
- **Commits:** `git -c core.autocrlf=false commit`, name files explicitly (do NOT `git add -A` — the tree has pre-existing untracked junk + the `.superpowers/` dir). No push.

---

### Task 1: Promote `fuzzer_mutate` → shared `components/fuzz_core`

**Files:**
- Create: `components/fuzz_core/fuzzer_mutate.h`, `components/fuzz_core/fuzzer_mutate.c` (moved), `components/fuzz_core/CMakeLists.txt`
- Delete: `applications/main/subghz/helpers/fuzzer_mutate.{c,h}`
- Modify: whatever component compiles the apps (the one whose `REQUIRES` must gain `fuzz_core`), and `tests/test_fuzzer_mutate.c` (include path).

**Interfaces:**
- Produces: `fuzz_core` component exposing `fuzzer_mutate.h` (`FuzzConfig`/`FuzzFrame`/`FuzzStrategy`/`FuzzMode`, `fuzzer_mutate_count`, `fuzzer_mutate_frame`) on the global include path for both subghz and ble_spam.

- [ ] **Step 1: Move the files** — `git mv applications/main/subghz/helpers/fuzzer_mutate.c components/fuzz_core/fuzzer_mutate.c` and same for `.h`. Content unchanged.

- [ ] **Step 2: Create `components/fuzz_core/CMakeLists.txt`**
```cmake
idf_component_register(
    SRCS "fuzzer_mutate.c"
    INCLUDE_DIRS "."
)
```

- [ ] **Step 3: Wire the dependency** — find the component that compiles `applications/main/**` (grep the repo for where `applications/main/subghz` sources are globbed — likely a top-level `main`/`flipper` component `CMakeLists.txt` with `idf_component_register(... REQUIRES ...)`). Add `fuzz_core` to that component's `REQUIRES` (or `PRIV_REQUIRES`). The include `#include "fuzzer_mutate.h"` in subghz's `fuzzer_frame.{c,h}` / `fuzzer_worker.c` then resolves via the component's include dir — leave those include strings unchanged.

- [ ] **Step 4: Fix the host test include path** — in `tests/test_fuzzer_mutate.c`, change `#include "../applications/main/subghz/helpers/fuzzer_mutate.c"` → `#include "../components/fuzz_core/fuzzer_mutate.c"`. (Also `tests/test_fuzzer_frame.c` includes `fuzzer_frame.c` which `#include "fuzzer_mutate.h"` — add `-I components/fuzz_core` to its gcc line.)

- [ ] **Step 5: Verify host tests still pass**
Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_fuzzer_mutate.c -o /tmp/tfm && /tmp/tfm && gcc -Wall -Icomponents/fuzz_core tests/test_fuzzer_frame.c -o /tmp/tff && /tmp/tff"`
Expected: both `TESTS_PASS`.

- [ ] **Step 6: Verify the subghz firmware build still green (clean reconfigure — component graph changed)**
PowerShell:
```
Remove-Item -Recurse -Force build_m5stick_s3 -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Expected: `SENTINEL_OK`; `nm build_m5stick_s3/furi_esp32.elf | grep fuzzer_mutate` still present (subghz fuzzer still links). If the app component can't see `fuzzer_mutate.h`, the REQUIRES wiring (Step 3) is wrong — fix it; **fallback** if component propagation to the fam-app proves intractable: instead of a component, move the files to a shared in-tree dir already on the app include path and adjust includes (report which you used).

- [ ] **Step 7: Commit**
```bash
git -c core.autocrlf=false add components/fuzz_core applications/main/subghz/helpers tests/test_fuzzer_mutate.c
git -c core.autocrlf=false commit -m "refactor(fuzz_core): promote fuzzer_mutate to shared component for subghz + ble reuse"
```

---

### Task 2: `ble_adv_decode` — advertising-data parser (host TDD)

**Files:**
- Create: `applications/main/ble_spam/ble_adv_decode.h`, `applications/main/ble_spam/ble_adv_decode.c`
- Test: `tests/test_ble_adv_decode.c`

**Interfaces:**
- Produces: `void ble_adv_decode(const uint8_t* adv, uint8_t len, BleAdvInfo* out)` filling a `BleAdvInfo` with flags, up-to-N 16-bit + 128-bit service UUIDs, tx_power (int8, present flag), and manufacturer id + data. Pure (no NimBLE deps) → host-testable. Consumed by the Target Info scene (Task 6).

- [ ] **Step 1: Write the failing test** (`tests/test_ble_adv_decode.c`)
```c
#include "../applications/main/ble_spam/ble_adv_decode.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // AD structures: [len][type][data...]
    // Flags(0x01)=0x06 ; 16-bit UUIDs(0x03)=0x180F,0x180A ; TXpower(0x0A)=0xF4(-12) ; MfrData(0xFF)=0x004C.....
    uint8_t adv[] = {
        0x02,0x01,0x06,
        0x05,0x03,0x0F,0x18,0x0A,0x18,
        0x02,0x0A,0xF4,
        0x03,0xFF,0x4C,0x00
    };
    BleAdvInfo info; memset(&info,0,sizeof info);
    ble_adv_decode(adv, sizeof adv, &info);
    assert(info.flags == 0x06);
    assert(info.uuid16_count == 2);
    assert(info.uuid16[0] == 0x180F && info.uuid16[1] == 0x180A);
    assert(info.has_tx_power && info.tx_power == -12);
    assert(info.mfr_present && info.mfr_id == 0x004C);
    // malformed: truncated length must not overread
    uint8_t bad[] = { 0x05,0x03,0x0F };   // claims 5 bytes, only 1 present
    BleAdvInfo i2; memset(&i2,0,sizeof i2);
    ble_adv_decode(bad, sizeof bad, &i2);   // must not crash / must bound-check
    assert(i2.uuid16_count == 0);
    printf("TESTS_PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run — expect FAIL** (`ble_adv_decode.c` missing).
Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_ble_adv_decode.c -o /tmp/tad 2>&1 | head"`

- [ ] **Step 3: Implement** — `ble_adv_decode.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define BLE_ADV_MAX_U16 8
#define BLE_ADV_MAX_MFR 24
typedef struct {
    uint8_t  flags; bool has_flags;
    uint16_t uuid16[BLE_ADV_MAX_U16]; uint8_t uuid16_count;
    uint8_t  uuid128[16]; bool has_uuid128;
    int8_t   tx_power; bool has_tx_power;
    uint16_t mfr_id; bool mfr_present;
    uint8_t  mfr_data[BLE_ADV_MAX_MFR]; uint8_t mfr_data_len;
} BleAdvInfo;
void ble_adv_decode(const uint8_t* adv, uint8_t len, BleAdvInfo* out);
```
`ble_adv_decode.c`: walk `[len][type][payload]` AD structures; bound every read by `len` (a claimed `ad_len` that runs past `len` terminates the walk — no overread); handle types 0x01 flags, 0x02/0x03 incomplete/complete 16-bit UUID lists, 0x06/0x07 128-bit, 0x0A TX power (int8), 0xFF manufacturer (first 2 bytes LE = id). Fill `out` (assume `out` is pre-zeroed by caller; the test does).

- [ ] **Step 4: Run — expect PASS** (`TESTS_PASS`).

- [ ] **Step 5: Commit**
```bash
git -c core.autocrlf=false add applications/main/ble_spam/ble_adv_decode.h applications/main/ble_spam/ble_adv_decode.c tests/test_ble_adv_decode.c
git -c core.autocrlf=false commit -m "feat(ble-fuzzer): advertising-data decoder (host-tested pure parser)"
```

---

### Task 3: `ble_walk_hal` fuzz/DoS primitives (firmware, compile-verified)

**Files:**
- Modify: `applications/main/ble_spam/ble_walk_hal.c`, `ble_walk_hal.h` (add, all `#ifndef PUBLIC_BUILD`)
- Reference: existing `ble_walk_hal_write_char` (uses `ble_gattc_write_flat`), `ble_walk_hal_connect`, and the unified GAP/GATT event cb in `ble_walk_hal.c`.

**Interfaces:**
- Produces (guarded): `bool ble_walk_hal_write_char_norsp(uint16_t handle, const uint8_t* data, uint16_t len)` (`ble_gattc_write_no_rsp_flat`); `bool ble_walk_hal_write_char_long(uint16_t handle, uint16_t offset, const uint8_t* data, uint16_t len)` (`ble_gattc_write_long` over an `os_mbuf`); `bool ble_walk_hal_fuzz_mtu(uint16_t preferred_mtu)` (`ble_att_set_preferred_mtu` + `ble_gattc_exchange_mtu`) + `uint16_t ble_walk_hal_get_negotiated_mtu(void)`; `bool ble_walk_hal_dos_connect_flood_start(const BleWalkDevice* target, uint16_t hold_count)` + `void ble_walk_hal_dos_flood_stop(void)` + `uint32_t ble_walk_hal_dos_counter(void)`. All operate against the currently-connected/locked target only.

- [ ] **Step 1: Implement** the primitives, guarded. Mirror `ble_walk_hal_write_char`'s conn-handle + return-code handling; for `write_long` build an `os_mbuf` via `ble_hs_mbuf_from_flat`; for MTU record the negotiated value in the existing GATT event cb's MTU branch; for the DoS flood reuse the connect internals with a churn loop + counter (respect an internal stop flag).

- [ ] **Step 2: Compile-verify (incremental)** — PowerShell:
```
$env:IDF_TOOLS_PATH="C:\Users\stone\.espressif"; $env:FLIPPER_BOARD="m5stick_s3"; $env:FLIPPER_VARIANT="private"
& "C:\Espressif\frameworks\esp-idf-v5.4.1\export.ps1" *> $null
Set-Location "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3"
idf.py -B build_m5stick_s3 build
```
Expected: "Project build complete" + fresh bin. (New HAL fns are unreferenced until Task 4 → may gc; success = compiles.)

- [ ] **Step 3: Commit** (`ble_walk_hal.c`/`.h`): `feat(ble-fuzzer): ble_walk_hal fuzz/DoS primitives (no-rsp/long-write/MTU/connflood, private)`

---

### Task 4: `ble_walk_fuzz` run engine — 3 modes, target-locked (firmware)

**Files:**
- Create: `applications/main/ble_spam/ble_walk_fuzz.h`, `ble_walk_fuzz.c` (guarded)
- Consumes: `ble_walk_hal` (Task 3 + existing connect/read), `fuzz_core` `fuzzer_mutate` (Task 1).

**Interfaces:**
- Produces (guarded): a run engine with `ble_walk_fuzz_alloc/_free`, `ble_walk_fuzz_set_target(const BleWalkDevice*)` (locks addr; arm refuses if unset), `ble_walk_fuzz_config(mode, FuzzConfig, char_handle, write_type, rate, count)`, `_start/_stop/_pause/_step`, `_running/_index/_counter`, `_set_callback(cb,ctx)` (per-iteration → view), `bool ble_walk_fuzz_mark_last(const char* path)`. Modes: `BleFuzzModeSweep` (Mode A: `fuzzer_mutate_frame` → `write_char[_norsp]`), `BleFuzzModeStack` (Mode B: MTU cycle over edge values + `write_char_long` length sweep), `BleFuzzModeDos` (Mode C: `dos_connect_flood` + max-rate `write_char_norsp`).

- [ ] **Step 1: Implement** the engine on its own FuriThread (mirror the Sub-GHz `fuzzer_worker` structure: start/stop/pause/step/callback, per-iteration stimulus, index/counter). Refuse `_start` if no target locked or (for A/B) no char handle set. Mark writes the full target block (use `ble_adv_decode` on the locked device's adv_data) + last payload + (mode/strategy/seed/index/handle/MTU) to the given path.

- [ ] **Step 2: Compile-verify (incremental)** — same PowerShell as Task 3 Step 2. Success = "Project build complete".

- [ ] **Step 3: Commit** (`ble_walk_fuzz.c`/`.h`): `feat(ble-fuzzer): 3-mode run engine (sweep/stack/DoS), target-locked (private)`

---

### Task 5: `ble_fuzz_view` run view (firmware)

**Files:**
- Create: `applications/main/ble_spam/views/ble_fuzz_view.h`, `ble_fuzz_view.c` (guarded)
- Reference: an existing ble_spam view or `subghz_view_spectrum.c` for the View alloc/draw/input pattern.

**Interfaces:**
- Produces (guarded): `ble_fuzz_view_alloc/_free/_get_view`, `_set_data(view, target_name, addr_str, mode_str, index, total, counter, tx_on)`, `_set_callback(view, cb, ctx)` + a `BleFuzzViewEvent` enum {StartPause, Step, Mark, Stop}.

- [ ] **Step 1: Implement** the draw (128x64): target name + addr; mode; `N/TOTAL` + counter; TX blink; footer key hints. Model on an existing view.

- [ ] **Step 2: Compile-verify (incremental)** — PowerShell `idf.py -B build_m5stick_s3 build` → "Project build complete".

- [ ] **Step 3: Commit** (`views/ble_fuzz_view.*`): `feat(ble-fuzzer): run view (private)`

---

### Task 6: Scenes + registration + menu entries (integration unit, private)

**Files (all guarded `#ifndef PUBLIC_BUILD`):**
- Create: `scenes/scene_walk_target_info.c` (full target-info panel via `ble_adv_decode` + "Arm mode" entry), `scenes/scene_walk_fuzz_config.c` (mode + strategy + write-type + rate + count → engine config), `scenes/scene_walk_fuzz_run.c` (owns `ble_walk_fuzz` engine + `ble_fuzz_view`, input→engine, Mark→`/ext/ble_fuzz/mark_<n>.txt`).
- Modify: `scenes/scene_config.h` (3 guarded `ADD_SCENE(ble_spam, walk_target_info, WalkTargetInfo)` / `walk_fuzz_config, WalkFuzzConfig` / `walk_fuzz_run, WalkFuzzRun`), `scenes/scene_walk_char_detail.c` (+ guarded "Fuzz" entry → WalkFuzzConfig with the selected char handle), `scenes/scene_walk_scan.c` (+ guarded "Target Info / DoS" entry on a selected device → WalkTargetInfo). Engine/view fields on the ble_spam app struct if needed (guarded).

**Interfaces:** Consumes Tasks 3–5. Produces the reachable private UI: scan → pick device → Target Info → arm (Fuzz A/B from char detail, DoS C from target info) → run view.

- [ ] **Step 1: Implement** all three scenes + the guarded `ADD_SCENE` lines + the two menu entries + any guarded app-struct fields. (Same coupling as the subghz fuzzer: scene bodies reference the `BleSpamSceneWalkFuzz*` enums that the `ADD_SCENE` lines generate — they land together.)

- [ ] **Step 2: Clean reconfigure build (scene_config changed) + verify presence**
```
Remove-Item -Recurse -Force build_m5stick_s3 -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Expected: `SENTINEL_OK`; `nm build_m5stick_s3/furi_esp32.elf | grep -E "ble_walk_fuzz|scene_walk_fuzz|scene_walk_target_info|ble_walk_hal_dos"` now PRESENT (reachable).

- [ ] **Step 3: Commit** (name the scene/config/view files + edited scenes explicitly): `feat(ble-fuzzer): target-info + fuzz/DoS scenes, registration + menu entries (private)`

---

### Task 7: Public gate — verify zero fuzz/DoS symbols on public

**Files:** none (verification only).

- [ ] **Step 1: Build public** (m5stick), background (full ~18 min):
```
Remove-Item -Recurse -Force build_m5stick_s3_public -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3_public.bat"
```
Expected: `SENTINEL_OK`.

- [ ] **Step 2: Assert 0 fuzz/DoS symbols + BLE Walk intact**
```
$nm = (Get-ChildItem "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe").FullName
& $nm build_m5stick_s3_public\furi_esp32.elf | Select-String "ble_walk_fuzz|scene_walk_fuzz|scene_walk_target_info|ble_walk_hal_dos|ble_walk_hal_fuzz"
& $nm build_m5stick_s3_public\furi_esp32.elf | Select-String "ble_walk_hal_connect"  # existing BLE Walk must still be present
```
Expected: first Select-String returns NOTHING (0 fuzz/DoS symbols, gc'd/guarded); second returns the existing BLE Walk symbol (explorer intact on public). Also confirm the private ELF HAS the fuzz/DoS symbols (Task 6 Step 2).

- [ ] **Step 3:** No commit (verification). Optionally flash the private m5stick/cardputer for on-device smoke (spec §8).

---

## Self-Review

**Spec coverage:** §2 reuse → Tasks 1/3/4; §2.1 target-lock + full info → Task 4 (lock) + Task 2 (adv decode) + Task 6 (target-info scene) + Task 4 (Mark); §3 Mode A → Task 4 (BleFuzzModeSweep) + Task 3 (writes); Mode B → Task 4 (Stack) + Task 3 (MTU/long); Mode C DoS → Task 4 (Dos) + Task 3 (connflood); §4 HAL → Task 3; §5 view/Mark → Tasks 5/4/6; §6 gating → guarded throughout + Task 7 gate; §7 scope → MVP tasks; §8 testing → host Tasks 1/2 + Task 7 gate + on-device note; §9 files → all tasks. Covered.

**Placeholder scan:** host tasks carry full test+impl code; firmware tasks give exact files, interfaces, references, and real build/verify commands (compile-gated, can't host-test). Task 1 documents a fallback if component propagation to the fam-app fails.

**Type consistency:** `BleAdvInfo` (Task 2) consumed by Tasks 4/6; `ble_walk_hal_*` primitives (Task 3) consumed by Task 4; `ble_walk_fuzz_*` engine (Task 4) + `ble_fuzz_view_*` (Task 5) consumed by Task 6; `BleWalkDevice.addr` (esp_bd_addr_t) used for the lock. `fuzzer_mutate` API unchanged by the Task 1 move.
