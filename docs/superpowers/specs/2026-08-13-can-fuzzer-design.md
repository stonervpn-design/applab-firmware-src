# CAN-Bus Fuzzer (CAN1) — design spec

**Date:** 2026-08-13
**Status:** Proposed — awaiting user approval
**Scope:** PRIVATE build only. Inherits CAN Commander's whole-app public exclusion.
**Tree:** `src_m5stick_s3`; new work in `applications/main/can_commander/`.
**Program:** gap-fill item CAN1 (see `docs/superpowers/redteam-gapfill-roadmap.md`).

## Goal
Add a **CAN-bus fuzzer** to the CAN Commander app: mutate a base `CanFrame`
(arbitration ID / DLC / data bytes / flags) and transmit the mutated frames on the
TWAI bus in a rate-limited, abortable loop, while surfacing the **real** TWAI
bus-state reaction (tx-fail / bus-error count / bus-off) after each burst. Purpose:
stress-test ECUs / CAN nodes and discover responsive arbitration IDs. CAN Commander
today does monitor + one-shot send + periodic send + presets — it has no fuzz/mutation
or ID-sweep engine.

## Constraints (hard)
- **PRIVATE-only — inherited, not re-implemented.** CAN Commander is excluded from
  public builds at the manifest level: `fam_config.py:272-274` removes
  `can_commander` from `APPS` when `_is_public`. A new scene inside this app is never
  compiled into public firmware, so it needs **no** `#ifndef PUBLIC_BUILD` guard (the
  app source uses none). Verification proves the whole app — including the fuzzer — is
  absent from the public ELF (`can_fuzz`/`can_commander` symbols = 0).
- **No new TX/driver path.** TX only via `can_hal_transmit(&frame, timeout)`
  (`can_hal.h:38`); start/stop via `can_hal_start(cfg)` / `can_hal_stop()` using the
  session `CanHalConfig` (`app->bitrate/mode/tx_gpio/rx_gpio`). No second
  `twai_transmit` call site.
- **Honest reaction indicator (no fake verdict).** Show the REAL TWAI state from
  `can_hal_stats()` (`tx_ok/tx_fail/bus_err/bus_off`); on `bus_off` call the existing
  `can_hal_recover()`. UI states plainly: bus state is a controller-level reaction
  (no-ACK can just mean "no device"), NOT a crash/vuln verdict.
- **DLC is hardware-bounded 0..8.** Classic CAN + TWAI cannot emit DLC>8 or
  below-spec timing; the fuzzer fuzzes what the controller can actually send and says
  so (no fake "malformed DLC" claims the hardware would never transmit).
- Fresh-log build method; host tests via WSL gcc (the `can_frame`/`can_fuzz` modules
  are furi/IDF-free and host-testable, per `can_frame.h`).

## Architecture
```
  can_frame.h (existing CanFrame + presets, furi-free)
                          │  base frame (preset / current tx_frame / default)
                          ▼
        helpers/can_fuzz.{c,h}  (NEW, pure, host-tested, furi/IDF-free)
          can_fuzz_next(const CanFrame* base, CanFuzzMode mode, uint32_t seed,
                        uint32_t step, CanFrame* out)
                          │  mutated CanFrame (id/dlc/data valid for TWAI)
                          ▼
   scenes/scene_fuzz.c  (NEW — clones scene_periodic.c's worker/abort/status)
     worker: mutate → can_hal_transmit → count; every N frames read can_hal_stats()
             → live tx_ok/tx_fail/bus_err/bus_off; bus_off → can_hal_recover()
```

## Components

### 1. Mutation core — `helpers/can_fuzz.{c,h}` (NEW, pure/furi-free, host-tested)
`void can_fuzz_next(const CanFrame* base, CanFuzzMode mode, uint32_t seed, uint32_t step, CanFrame* out);`
- Deterministic `splitmix32(seed ^ step)` (same idiom as the nRF24 / WiFi fuzzers).
- `CanFuzzMode`:
  - `CFZ_DATA_BITFLIP` — copy base, flip one bit in `data[0..dlc-1]` (if `dlc>0`).
  - `CFZ_DATA_RANDOM` — copy base id/flags, randomize all `dlc` data bytes.
  - `CFZ_ID_SWEEP` — keep base data/dlc, set `id = step % (range+1)` where range is
    `CAN_ID_STD_MAX` (0x7FF) or `CAN_ID_EXT_MAX` (0x1FFFFFFF) per `base->extended`.
    (The ECU-discovery / targeted-flood mode.)
  - `CFZ_DLC_SWEEP` — keep base id, set `dlc = step % 9` (0..8), fill new data bytes
    from the PRNG.
  - `CFZ_FULL_RANDOM` — randomize id (masked to the valid 11/29-bit range for
    `base->extended`), `dlc` (0..8), and data.
- **Every output is a valid transmittable frame:** `dlc` clamped to [0,8]; `id`
  masked to `CAN_ID_STD_MAX`/`CAN_ID_EXT_MAX`; `extended`/`rtr` preserved from base
  (RTR frames carry no data — respected). Never produces a frame `can_hal_transmit`
  would reject, so the counter reflects real bus traffic.

### 2. Fuzz scene — `scenes/scene_fuzz.c` (NEW)
- Clone `scene_periodic.c`: `FuriThread* fuzz_thread` + `volatile bool fuzz_run`,
  a stepped-sleep loop (checks `fuzz_run` every ≤20 ms) at a selectable interval
  (reuse periodic's `{10,20,50,100,500}` ms set), `can_*_stop()` helper joined+freed
  in `on_exit`. Prereqs on Start: `can_hal_start(cfg)` if not running.
- **Base frame source:** the app's current `tx_frame` (set via Send/Presets), else a
  default (id 0x100, dlc 8, incrementing data). A VariableItem picks the **mode**.
- Worker loop: `can_fuzz_next(&base, mode, seed, step++, &f)` → `can_hal_transmit(&f, 20)`
  → count on success. Every ~200 ms (on Tick) read `can_hal_stats()` and render:
  `Sent N | fail F | busErr E | BUS-OFF`. If `bus_off`, call `can_hal_recover()` once
  and note "recovering".
- Header/status text (honest): "Bus state = real TWAI counters (no-ACK may just mean
  no device). Not a crash verdict."
- Register `ADD_SCENE(can_commander, fuzz, Fuzz)` in `can_commander_scene_config.h`;
  add a "Fuzzer" submenu item + custom event + dispatch in `scene_main.c` /
  `can_commander_custom_event.h`. **No `#ifndef PUBLIC_BUILD`** (whole-app exclusion).

## Data flow
base (tx_frame/default) → config (mode) → run worker: can_fuzz_next(mode, step++) →
can_hal_transmit → count; Tick → can_hal_stats() → live bus-state; bus_off → recover.

## Testing
- **Host (WSL gcc, pure):** `tests/test_can_fuzz.c` — determinism (same seed+step →
  identical frame); every mode yields `dlc∈[0,8]` and `id ≤ max-for-width`; BITFLIP
  flips exactly one data bit and only within `data[0..dlc-1]`; ID_SWEEP is monotonic
  mod range and stays within std/ext bounds; DLC_SWEEP cycles 0..8; RTR base keeps
  RTR; determinism across all modes. Compiles with `-Iapplications/main/can_commander`
  + `can_frame.c` (furi-free).
- **Build gate:** private m5stick + cardputer green (fresh logs). **Public exclusion:**
  build public, confirm `SENTINEL_OK`+`NOCAR PASS`, and nm/map scan of the public ELF
  for `can_fuzz`/`can_commander`/`can_hal` → **0** (proves the whole car app, fuzzer
  included, is absent from public — same guarantee as WF1's 0-symbol proof, here via
  manifest exclusion).
- **HW (deferred, needs a CAN bus + transceiver):** real Grove CAN transceiver on
  GPIO2/1, a live bus/ECU. Expect the bus-state indicator to be the real proof point.

## Honest limits (surfaced in-UI / docs)
- **Needs an external CAN transceiver** on the Grove port (GPIO2 TX / GPIO1 RX); the
  ESP32 TWAI controller alone is not a bus driver.
- **DLC 0..8, standard CAN timing only** — TWAI won't emit DLC>8 or out-of-spec
  frames; the fuzzer fuzzes the transmittable space and says so.
- **Bus-state ≠ crash verdict** — `tx_fail`/`bus_err`/`bus_off` are real controller
  counters; "no ACK" can simply mean no other node is present. Reported honestly.
- **ID_SWEEP over 29-bit ext space is astronomically large** — it sweeps sequentially
  from 0; the UI shows the current ID so the operator knows how far it has reached.

## Non-goals (YAGNI)
- No new appid (lives in can_commander, inherits its exclusion). No new TX/driver path.
- No ISO-TP / UDS diagnostic session logic (raw-frame fuzzing only in v1).
- No on-device "did the ECU crash?" oracle beyond the real TWAI bus-state counters.
- No DLC>8 / CAN-FD (hardware classic-CAN only).

## Success criteria
- Fuzzer reachable from the CAN Commander menu (private build), 5 mutation modes,
  seeds from current frame/default, rate + abort, live REAL bus-state indicator.
- `can_fuzz` mutators pass host unit tests (determinism + all frames TWAI-valid).
- Private m5stick + cardputer green; public build excludes the whole can_commander app
  (0 `can_fuzz`/`can_commander` symbols) — fresh-verified.
- Honest limits (transceiver required, DLC≤8, bus-state≠verdict) surfaced in-UI.
