# Sub-GHz Protocol Fuzzer — Design

**Date:** 2026-08-11
**Status:** Approved design, pre-implementation
**Scope:** Private-only (`#ifndef PUBLIC_BUILD`) red-team tool in the M5Stick S3 / Cardputer ADV Flipper port.
**Boards:** m5stick_s3 (2-button + tilt), m5stack_cardputer_adv (keyboard). Both use CC1101 + the existing SubGHz TX path.

## 1. Purpose

Transmit deliberately malformed / mutated Sub-GHz frames to probe a *receiver's* parser for
faults — misfires, unintended actuation (unlock/open), state resets, or hangs — during the
owner's own authorized RF testing. Fills the "protocol fuzzing" gap in the existing offensive
Sub-GHz surface (SubBrute, RollBack, jammer, ProtoPirate all send *well-formed* frames; nothing
sends *malformed* ones).

Non-goals: not a rolling-code/crypto attack (blind mutation of KeeLoq is low-value), not an
automated vulnerability scanner. It is an **open-loop** fuzzer: the operator observes the physical
target; the tool's job is to emit reproducible mutated frames and let the operator pinpoint and
save the one that triggered a fault.

## 2. Fuzzing modes (target models)

Two modes, selectable in config:

1. **RAW bitstream fuzz** (protocol-agnostic). Operator picks frequency + modulation preset
   (OOK650 / AM270 / FM238 / etc.) + base timing `te`. The fuzzer emits RAW duration-array frames
   (`Protocol: RAW`, `Encrypt_data: RAW`-style) with mutated pulse/gap sequences. Tests the
   demodulator + framing layer, independent of any protocol decoder.

2. **Protocol-structured fuzz**. Operator picks a known **fixed-code** protocol and a seed frame
   (default or loaded `.sub`). The fuzzer mutates the *decoded fields* (key bits, bit-length,
   checksum, button/serial values), then re-encodes + transmits through that protocol's existing
   encoder. Tests a specific parser. MVP protocol set: Princeton/PT2262, CAME, NICE FLO (fixed-code),
   Holtek, Linear. (Rolling-code/crypto protocols deferred — see §8.)

## 3. Mutation strategies

Applied to the seed frame (a bit vector for protocol mode, a duration array for RAW mode):

- **Bit-flip sweep** — flip each bit in turn; N frames for N bits (fully deterministic, index = bit).
- **Boundary/edge** — all-zeros, all-ones, min-length, max-length, nominal ± 1 bit count.
- **Length fuzz** — vary bit-count / duration-count around nominal by a configured range.
- **Random** — seeded PRNG corruption of K bits/bytes per frame, for a configured iteration count.
  Reproducible by (seed, index).
- **Field-targeted** (protocol mode only) — walk one field (serial / button / counter) through a
  value range while holding the rest of the frame constant.

Each emitted frame has a deterministic identity: **(strategy, seed, index)**. This is the contract
that makes a fault reproducible.

## 4. Delivery / worker

New `fuzzer_worker` modeled on `applications/main/subghz/helpers/subbrute_worker.c`:

- Owns the TX loop on its own thread (start / stop / pause / step / callback), mirroring
  `subbrute_worker_start/stop` and `subbrute_worker_send_callback`.
- For each frame: build a `FlipperFormat` string (RAW duration array for RAW mode; protocol + bits +
  key + preset + te + repeat for protocol mode — same construction SubBrute already uses), then
  transmit via the existing path (`subghz_txrx_tx_start` / the SubBrute
  `subbrute_worker_subghz_transmit` deserialize→`subghz_devices_start_async_tx` sequence). **No new
  radio code.**
- Cadence: configurable `repeat` per frame and inter-frame delay, reusing SubBrute's timing knobs.
- Run modes: **auto-sweep** (advance automatically at the configured rate) and **single-step**
  (advance exactly one frame per operator keypress).

## 5. Reproducibility (the core feature)

Open-loop ⇒ no auto crash detection; reproducibility is what makes it useful:

- Run view shows **live frame index / total** and the current mutation descriptor.
- **Single-step** lets the operator advance one frame at a time to pinpoint the trigger.
- **Mark** dumps the last-sent frame (and a small ring buffer of the previous few) to
  `/ext/subghz/fuzzer/` as standard `.sub` files, so the fault can be replayed with the normal
  Transmitter / inspected in Signal Decoder.
- On Stop, the tool records the last (strategy, seed, index) so a session can be resumed / a range
  re-run.

## 6. UI / scenes

All new scenes gated `#ifndef PUBLIC_BUILD`.

- **Menu entry:** `SubmenuIndexProtocolFuzzer` "Protocol Fuzzer" in
  `applications/main/subghz/scenes/subghz_scene_start.c`, near Bruteforce / Jammer / RollBack, and a
  matching `ADD_SCENE(subghz, protocol_fuzzer_config, ...)` in `subghz_scene_config.h` — both
  `#ifndef PUBLIC_BUILD` (matches how jammer/rollback are gated).
- **Config scene** (`subghz_scene_protocol_fuzzer_config.c`): VariableItemList — mode (RAW/Protocol),
  protocol (protocol mode), frequency, modulation preset, seed source (default / load `.sub`),
  mutation strategy, rate, repeat, run mode (auto/step).
- **Run scene + view** (`subghz_scene_protocol_fuzzer_run.c` + `views/subghz_view_fuzzer.c`): shows
  freq, mode, mutation descriptor, frame #/total, a TX blink indicator, and controls
  Start‑Pause / Step / Mark / Stop plus a small list of marked frames.

### Input mapping

- **Cardputer (keyboard):** direct keys — Space=Start/Pause, →=Step, M=Mark, Esc/Back=Stop.
- **M5Stick S3 (2-button + tilt):** tap = Step, hold KEY1 = Start/Pause, KEY2 = Back/Stop; Mark =
  auto-save-on-stop plus a tap-while-paused combo (matches the port's `BOARD_TWO_BUTTON_INPUT`
  scheme). Flagged as the tightest UX; RAW/auto-sweep is the primary M5Stick flow.

## 7. Safety / gating

- **Private-only.** App + scenes + menu item + worker all `#ifndef PUBLIC_BUILD`; verify the public
  ELF has zero fuzzer symbols (gc'd) after build, same as jammer/rollback.
- **Frequency allowlist.** Reuse the existing SubGHz TX frequency validation so the fuzzer cannot
  transmit outside the port's permitted bands / region.
- **In-app authorized-use note** on first entry (matches the jammer's framing). Transmitting
  malformed RF is regulated; this is for the owner's own authorized testing only.

## 8. MVP scope vs deferred

**MVP (this build):**
- RAW bitstream fuzz + structured fuzz for the 5 fixed-code protocols in §2.
- Bit-flip / boundary / length / random mutations; field-targeted for protocol mode.
- Auto-sweep + single-step; Mark-to-`.sub`; frequency allowlist; private gating.

**Deferred (v2, not built now):**
- Rolling-code / crypto protocol field fuzzing (KeeLoq, Security+ 2.0, etc.).
- RX-after-TX response auto-detection (listen window for target ACK / state change).
- Coverage heuristics, corpus management, session resume UI beyond last-index.

## 9. Testing

- **Host unit tests** for the pure logic (no radio): mutation generators are deterministic given
  (strategy, seed, index) — assert bit-flip sweep covers each bit exactly once, boundary set is
  correct, random is reproducible per seed, length-fuzz stays in range, field-targeted walks the
  intended range. Add under `tests/` mirroring `tests/test_subghz_decode_dedup.c`.
- **On-device smoke:** RAW mode against a bench receiver / SDR to confirm frames actually emit at the
  selected freq/preset; structured mode against a known fixed-code remote's receiver; confirm Mark
  writes a replayable `.sub`.
- **Public-build gate:** build public, `nm` the ELF → 0 fuzzer symbols; car-free gate still PASS.

## 10. Files (planned)

New (private):
- `applications/main/subghz/helpers/fuzzer_worker.{c,h}` — TX loop + frame gen (mirror subbrute_worker).
- `applications/main/subghz/helpers/fuzzer_mutate.{c,h}` — pure mutation generators (host-testable).
- `applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_config.c`
- `applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_run.c`
- `applications/main/subghz/views/subghz_view_fuzzer.{c,h}`
- `tests/test_fuzzer_mutate.c`

Edited (guarded `#ifndef PUBLIC_BUILD`):
- `applications/main/subghz/scenes/subghz_scene_start.c` — menu item + handler.
- `applications/main/subghz/scenes/subghz_scene_config.h` — ADD_SCENE x2.
- `applications/main/subghz/subghz_i.h` — SubmenuIndexProtocolFuzzer + view id if needed.
- CMake/globs pick up the new sources automatically (component globs).
