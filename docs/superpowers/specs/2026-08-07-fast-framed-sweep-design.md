# P2a: Fast Framed Sweep (design)

**Date:** 2026-08-07
**Status:** approved (design), pending plan
**Scope:** PRIVATE builds only (public behavior unchanged). Second roadmap item toward matching the PandwaRF Rogue Pro; makes the existing SubBrute framed brute ~2x faster.

---

## Goal

Cut the wall-clock of a SubBrute framed sweep by roughly half, with a user toggle and a safe fallback. Every framed attack (CAME/NICE/Holtek/Chamberlain/Linear/Princeton…) benefits.

## Findings that shape this (from the P2 research workflow)

- Per-code today ≈ **204 ms** = ~182 ms RF airtime (Repeat=4 × ~44.5 ms frame) + ~22 ms removable MCU overhead.
- The framed brute is **bounded**, not "infeasible": `subbrute_protocol_calc_max_value` hard-caps Princeton/PT2262 at **6561** (3⁸ tri-state), so a real Princeton sweep ≈ **19 min** today.
- ~89% of each code is **RF airtime you cannot remove**: the PT2272 receiver needs the frame's guard gap (≥15·te) **and** ≥2 repeats to actuate. Framed brute can be ~2× faster, not instant — the de Bruijn path is the "instant" tool and already exists.

## Non-goals (deferred follow-ons)

- Continuous single-burst streaming (the de Bruijn mechanism — one `start_async_tx`, allocation-free inline-PWM yield, no per-code re-arm). Higher risk, smaller marginal win; noted for later.
- ISR event-wait (FuriEventFlag) replacing the completion poll — HAL change, deferred.
- Transmitter / RMT-channel reuse across codes — deferred.
- Persisting the Fast toggle across reboots (settings-file change) — v1 keeps it a session flag.

---

## GLOBAL CONSTRAINTS

1. **Public behavior unchanged.** `fast_mode` defaults **true in private, false in public** via one `#ifdef` at worker init; the Fast toggle UI is `#ifndef PUBLIC_BUILD`. Public never enables fast mode, so the framed brute behaves exactly as today (full repeats, per-code config). The fast branches compile in both but are inert in public (they are timing logic, not sensitive material). Verified in the gate task.
2. **Receiver-safety floor:** fast mode clamps repeats to **2** (never below) and never touches the frame's guard gap — the two things a real receiver needs to latch.
3. **Bench-authorized only:** continuous keyed TX has duty/thermal cost; the sweep stays cancelable (Back).

---

## Design — three changes behind a `fast_mode` flag

Per-code cost 204 ms → ~95 ms.

1. **Repeat clamp (biggest lever, ~−89 ms).** The per-code payload is built with `instance->repeat`. In fast mode pass `min(repeat, 2)` instead. Halves airtime. 2 is the safe latch floor.
2. **Configure the radio once per attack (~−9.5 ms).** `subbrute_worker_subghz_transmit` re-runs `reset/idle/load_preset/set_frequency` (subbrute_worker.c:465-468) every code, but freq/preset are invariant across a sweep (the de Bruijn path proves configure-once is safe). Gate that block on `!fast_mode || !radio_ready`; set `radio_ready` after the first configure; reset it per attack and on manual TX.
3. **Trim the dead time (~−8 ms).** The completion busy-poll (subbrute_worker.c:476) and the inter-code delay (subbrute_worker.c:585) both use `tx_timeout_ms` (6 ms). In fast mode use 1 ms for both.

**Fast toggle:** a "Fast" On/Off item in `subghz_scene_bf_setup_extra.c` (a `VariableItemList`), private-only. Stored as a **session flag on the worker** (`subbrute_worker_set_fast` / `_is_fast`) — no settings-file change. Default matches the build (`#ifdef`). OFF restores the exact original path (per-code config, full repeats, 6 ms delays) for a stubborn receiver.

---

## Components & exact call sites

### MODIFY `helpers/subbrute_worker_private.h`
- Add `bool fast_mode;` and `bool radio_ready;` to `struct SubBruteWorker`.

### MODIFY `helpers/subbrute_worker.c`
- `#define SUBBRUTE_FAST_REPEAT 2` near the top.
- In `subbrute_worker_alloc`: `#ifndef PUBLIC_BUILD instance->fast_mode = true; #else instance->fast_mode = false; #endif`, `instance->radio_ready = false;`.
- `subbrute_worker_subghz_transmit` (432-499): local `uint8_t poll_ms = instance->fast_mode ? 1 : instance->tx_timeout_ms;` used at the two poll waits (435, 476); wrap the config block (465-468) in `if(!instance->fast_mode || !instance->radio_ready){ …; instance->radio_ready = true; }`.
- `subbrute_worker_thread` sweep loop (540-586): compute `uint8_t rep = (instance->fast_mode && instance->repeat > SUBBRUTE_FAST_REPEAT) ? SUBBRUTE_FAST_REPEAT : instance->repeat;` and pass `rep` to `subbrute_protocol_default_payload` / `subbrute_protocol_file_payload` in place of `instance->repeat`; set `instance->radio_ready = false;` before the `while`; inter-code delay (585) → `furi_delay_ms(instance->fast_mode ? 1 : instance->tx_timeout_ms);`.
- `subbrute_worker_transmit_current_key` (manual TX ~302): `instance->radio_ready = false;` before transmit so single-shot always re-configures.
- Add `void subbrute_worker_set_fast(SubBruteWorker*, bool)` + `bool subbrute_worker_is_fast(SubBruteWorker*)`.

### MODIFY `helpers/subbrute_worker.h`
- Declare the two accessors.

### MODIFY `scenes/subghz_scene_bf_setup_extra.c` (`#ifndef PUBLIC_BUILD`)
- Add a "Fast" `VariableItem` (Off/On) whose change callback calls `subbrute_worker_set_fast(subghz->subbrute_worker, index==1)`; initialise its index from `subbrute_worker_is_fast`.

---

## Safety

- Read paths untouched. The sweep already requires the user to start it and can be cancelled (Back). Fast mode only changes timing + repeat count.
- Clamp floor 2 + untouched guard gap keep real-receiver actuation intact.
- Public gets none of it at runtime.

## Testing & verification

1. **Private build:** green; `nm` shows `subbrute_worker_set_fast` PRESENT; on-device smoke (deferred to user): a CAME/Princeton sweep visibly ~2× faster; toggle OFF restores prior cadence.
2. **Public gate:** green; `fast_mode` initialised false (verify the `#ifdef`); the "Fast" toggle string absent from the public bin; `verify_public_no_car` still PASS. Public framed-brute behavior identical to before.

## Deferred follow-ons (noted, not in v1)

Continuous single-burst streaming; ISR event-wait; transmitter/RMT reuse; persisted toggle. Each is an isolated future step; the airtime floor + guard/repeat physics bound any framed approach.
