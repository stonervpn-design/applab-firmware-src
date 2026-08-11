# BLE GATT Fuzzer + DoS — Design

**Date:** 2026-08-11
**Status:** Approved design, pre-implementation
**Scope:** Private-only (`#ifndef PUBLIC_BUILD`) red-team tool extending the existing **BLE Walk** GATT explorer in the `ble_spam` app. M5Stick S3 / Cardputer ADV Flipper port; ESP32-S3 onboard BLE (NimBLE, central role enabled).
**Authorized-use only:** BLE is short-range/single-target. For assessing devices you own or are explicitly authorized to test. Never ships in a public build.

## 1. Purpose

The port already has a BLE GATT **explorer** — `ble_spam`'s "BLE Walk" (`ble_walk_hal.c` + `scene_walk_scan/services/chars/char_detail`): scan → connect → discover services → discover characteristics → read → basic write (fixed `0x01`). This feature adds the missing **offensive** layer on top of that proven GATT client:

- **Mode A — Mutation-sweep fuzz:** write mutated payloads to a writable characteristic to find parser/app-logic bugs.
- **Mode B — MTU / long-write fuzz:** probe the GATT *stack* with edge MTU exchanges and oversized/prepared long-writes.
- **Mode C — BLE DoS:** connection-exhaustion flood + max-rate GATT write flood to test a target's availability/resilience (complements `ble_spam`'s existing advertising-layer crash-flood).

Non-goals (v1): notify/CCCD feedback (open-loop like the Sub-GHz fuzzer), custom byte-input manual write, deep prepared-write offset-overflow matrices. All three modes are OPEN-LOOP — the operator observes the target; the tool's job is reproducible stimulus + Mark-to-save.

## 2. Architecture & reuse

- **Reuse `ble_walk_hal`** (scan/connect/discover/read/write) unchanged for the scan→connect→pick-characteristic plumbing. Add fuzz/DoS primitives to it (§4).
- **Reuse the mutation engine.** Promote `fuzzer_mutate.{c,h}` (built for the Sub-GHz fuzzer, pure/host-tested — bit-flip / boundary / length / random / field-walk over a byte vector) into a **shared component `components/fuzz_core/`**. Repoint the Sub-GHz fuzzer's `#include "fuzzer_mutate.h"` (the component goes on the include path, so the include string is unchanged — verify the subghz build still green after the move). `ble_spam` then `REQUIRES fuzz_core` in its CMakeLists. This keeps mutation logic DRY across both fuzzers and avoids an `ble_spam → subghz` dependency.
- **New UI** in `ble_spam`: `scene_walk_fuzz_config` + `scene_walk_fuzz_run` (+ a small run view), reached via a "Fuzz / DoS" action added to `scene_walk_char_detail` (for A/B, char-scoped) and to the device/scan level (for C, target-scoped). All `#ifndef PUBLIC_BUILD`.

## 3. Modes

### Mode A — Mutation-sweep write fuzz (parser bugs)
From a selected **writable** characteristic: config = strategy (bit-flip / boundary / length / random), seed (the characteristic's last-read value, else a default), write type (with-response `write_flat` / no-response `write_no_rsp`), rate, count. Run loop: `fuzzer_mutate_frame(cfg, index)` → payload bytes → GATT write to the char handle. Single-step + Mark.

### Mode B — MTU / long-write fuzz (stack bugs)
- **MTU:** cycle preferred-MTU edge values (23 min, 517 max, and out-of-range e.g. 0 / 0xFFFF) via `ble_att_set_preferred_mtu(mtu)` then `ble_gattc_exchange_mtu(conn)`; record the negotiated result / errors.
- **Long-write:** `ble_gattc_write_long(conn, handle, offset, om)` with payload lengths increasing past the characteristic's declared max and varied offsets — reuses the mutation engine's Length strategy extended to large sizes. Probes the server's prepared-write buffer handling.

### Mode C — BLE DoS (availability)
- **Connection-exhaustion flood:** repeatedly `ble_gap_connect` the target (hold N connections open + churn) to fill its connection table so legitimate clients can't attach. Knobs: connect rate, hold count.
- **GATT write flood:** once connected, max-rate `write_no_rsp` to a chosen characteristic to saturate GATT processing (this is the Mode-A run loop with mutation disabled and rate maxed — same run engine).
- Clear **Stop** + live packet/connection counter; in-app authorized-use note.

## 4. `ble_walk_hal` additions (new fuzz/DoS primitives)

```c
// no-response fast write (sweeps / flood)
bool ble_walk_hal_write_char_norsp(uint16_t handle, const uint8_t* data, uint16_t len);
// prepared/long write (offset + length past attr max)
bool ble_walk_hal_write_char_long(uint16_t handle, uint16_t offset, const uint8_t* data, uint16_t len);
// MTU fuzz: set local preferred MTU then trigger exchange; poll result
bool ble_walk_hal_fuzz_mtu(uint16_t preferred_mtu);
uint16_t ble_walk_hal_get_negotiated_mtu(void);
// DoS: connection flood control (reuses connect/disconnect internals)
bool ble_walk_hal_dos_connect_flood_start(BleWalkDevice* target, uint16_t hold_count);
void ble_walk_hal_dos_flood_stop(void);
uint32_t ble_walk_hal_dos_counter(void);   // conns opened / writes sent
```
(`write_char_norsp` → `ble_gattc_write_no_rsp_flat`; `write_char_long` → `ble_gattc_write_long`; both standard NimBLE, same header as the `ble_gattc_write_flat` BLE Walk already uses. Verify signatures at impl.)

## 5. Reproducibility & UX

- Run view (128x64): target name/addr, mode, current char UUID/handle, frame/packet index + total, TX blink, counter; controls Start‑Pause / Step / Mark / Stop (Cardputer keyboard direct; M5Stick 2-button tap=Step / hold=Start‑Pause / KEY2=Stop, Mark=auto-save-on-stop — mirrors the Sub-GHz fuzzer input note).
- **Mark** dumps the last-sent payload + (mode, strategy, seed, index, char handle) to `/ext/ble_fuzz/mark_<n>.txt` on SD so a fault is reproducible.
- Open-loop: no automatic crash detection; operator observes the target.

## 6. Gating (private-only inside a public app)

`ble_spam` ships on public (Defender links its BLE-walk/tracker scenes), so guard ONLY the new fuzz/DoS surface `#ifndef PUBLIC_BUILD`: the new HAL functions, the two new scenes + view, the "Fuzz / DoS" menu entries + their scene enums/handlers, and the `fuzz_core` include in `ble_spam`. Existing BLE Walk (scan/read/test-write) is unchanged and stays public. **Verify:** public build green + `nm` shows zero `ble_walk_hal_dos_*` / `scene_walk_fuzz_*` symbols; private has them.

## 7. Scope

**MVP:** Modes A (with-rsp + no-rsp), B (MTU cycle + long-write length sweep), C (connect flood + write flood); Mark-to-SD; shared `fuzz_core`; private gating.
**Deferred (v2):** notify/CCCD feedback, custom byte-input manual write, prepared-write offset-overflow matrices, multi-target/parallel DoS.

## 8. Testing

- **Host:** `fuzz_core` mutation tests already exist (moved with the component — re-run under WSL gcc to confirm the relocation didn't break the include). No new pure logic needs host tests beyond the DoS/flood counters (a small host-testable rate/counter helper if extracted).
- **On-device smoke:** against a spare BLE peripheral you own — Mode A writes a mutated sweep to a writable char; Mode B runs MTU cycle + an oversized long-write; Mode C connection-floods and the target becomes unreachable to a second client; confirm Mark writes a reproducible log.
- **Public gate:** public build green, `nm` zero fuzz/DoS symbols, existing BLE Walk intact.

## 9. Files (planned)

Move: `applications/main/subghz/helpers/fuzzer_mutate.{c,h}` → `components/fuzz_core/fuzzer_mutate.{c,h}` (+ `components/fuzz_core/CMakeLists.txt`); repoint subghz include path (component on include path → include string unchanged); `ble_spam/CMakeLists.txt` (or its `.fam`) `REQUIRES fuzz_core`.
New (private, in `applications/main/ble_spam/`): `ble_walk_fuzz.{c,h}` (fuzz/DoS run engine over ble_walk_hal + fuzz_core), `scenes/scene_walk_fuzz_config.c`, `scenes/scene_walk_fuzz_run.c`, `views/ble_fuzz_view.{c,h}`.
Edit (guarded `#ifndef PUBLIC_BUILD`): `ble_walk_hal.{c,h}` (new primitives §4), `scenes/scene_walk_char_detail.c` (+ "Fuzz" entry), `scenes/scene_walk_scan.c` or device scene (+ "DoS" entry), `scenes/scenes.h` + `scenes/scene_config.h` (scene registration).
