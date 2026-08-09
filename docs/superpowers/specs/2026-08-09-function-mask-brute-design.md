# Function-Mask Bit-Granular Brute — Design (2026-08-09)

PRIVATE-only, authorized research. The roadmap's adversarially-vetted build #3
(P2b) — the one genuine feature PandwaRF has that fzcard lacks in general form.
Design/spec only; no code yet.

## Goal

Brute only the UNKNOWN bits of a fixed-code frame, holding known bits fixed —
`data = (capture AND ~mask) OR (enumerated & mask)` — at arbitrary BIT (and, for
Princeton, TRIT) granularity, not byte-aligned. Reduces the search from 2^N over
all bits to 2^k over the k masked bits. Fixed-code protocols only.

## Why (honest value + limits)

- **Value:** enumerate only the don't-know bits — un-captured button/function
  codes for a known serial, or don't-care-bit receivers — turning a days-long
  full sweep into seconds when the serial is fixed from a capture. Chip-ID
  (P1 [[fzcard-chip-id]]) auto-seeds the fixed address bits.
- **NOT first access:** a clean capture is already replayable; this is for
  codes you did NOT capture. Marginal but real — exactly PandwaRF's niche.
- **GUARDRAIL:** holding a known serial fixed collapses a gate's space to
  seconds. MUST stay `#ifndef PUBLIC_BUILD` + `nm`-verified out of every public
  ELF/webflasher, same as the rest of the brute surface.

## Current engine (what we extend)

`applications/main/subghz/helpers/subbrute_device.c` / `subbrute_protocols.c`:
the worker walks `current_step` from 0 to `max_value` and, per step, builds a
candidate frame (`subbrute_protocol_create_candidate_for_existing_file`) —
today byte-granular, whole-word enumeration. Princeton PT2260/PT2262 tri-state
protocols already exist in the table (trit LUTs). We generalize the step→frame
mapping to scatter the step across only masked positions.

## Architecture

- **`SubBruteFuncMask`** (new struct): `uint64_t fixed_value` (known bits from
  the capture), `uint64_t var_mask` (1 = enumerate this bit), `bool tri_state`
  (Princeton: enumerate in 0/1/float trit-space over masked trits), `uint8_t
  bit_count`. `k = popcount(var_mask)` (or masked-trit count); `max_value = 2^k`
  (or `3^k_trit`).
- **Pure candidate generator** (`subbrute_mask_candidate(fixed, mask, step)`):
  deposit the k low bits of `step` into the k masked bit positions (LSB→lowest
  set mask bit), OR with `fixed & ~mask`. For tri-state, base-3 decompose `step`
  and map each trit to the protocol's 0/1/float symbol LUT. Host-testable,
  furi-free — a Python replica proves the step→frame mapping is a bijection over
  masked positions and never touches fixed bits.
- **Chip-ID auto-mask:** given a decoded fixed-code capture + its chip family
  (P1), seed `fixed_value` from the capture and default `var_mask` to the
  button/function field (address bits fixed, data/button bits variable) — one
  button press to a full mask.
- **Mask editor UI:** a scene extending `bf_setup` — a bit/trit grid where each
  position toggles fixed(from capture)/variable; live `2^k` code-count + ETA at
  the current sweep rate (reuse the fast-sweep timing from [[fzcard-pandwarf-roadmap]] P2a).
- Wire into the existing worker: `max_value = 2^k`, and route
  `create_candidate_for_existing_file` through `subbrute_mask_candidate` when a
  mask is active (byte-granular path unchanged when no mask).

## Phased plan (each host-testable, gate-checked, private-only)

- **Phase 1 — pure mask candidate generator + host test.** `subbrute_mask.{h,c}`
  (`subbrute_mask_candidate`, popcount/max-value helpers), furi-free. Python
  replica `tools/subbrute_mask/mask_replica.py` proves: bijection over masked
  bits, fixed bits never altered, `max_value == 2^k`. Binary + tri-state cases.
- **Phase 2 — worker integration.** Route the worker's candidate build through
  the mask generator when a mask is set; `max_value = 2^k`. Verify the existing
  byte-granular sweep is byte-identical when no mask (regression).
- **Phase 3 — chip-ID auto-mask seed.** From a decoded capture + chip family,
  seed `fixed_value` + default `var_mask`. Host-test the address/data split per
  chip.
- **Phase 4 — mask editor scene + ETA.** Bit/trit toggle grid in bf_setup, live
  2^k + ETA. `#ifndef PUBLIC_BUILD` at scene_config + menu + all sites.
- **Phase 5 — guardrail + gate.** `nm`-verify 0 mask symbols in public; extend
  `verify_public_no_car.py` (or the attack-symbol denylist) with the mask
  symbols; mirror is automatic (superset tree → cardputer via build_adv).

## Verification (no host gcc)

Pure generator via Python replica + `xtensa-esp32s3-elf-gcc -fsyntax-only`;
symbol presence via `nm`; public gc-gate every build; on-device sweep confirms
every masked code actually keys (deferred to HW test, like the rest of the brute
surface).

## Open questions for the owner

1. Default mask from chip-ID = button/function field only (address fixed), or
   also expose a "free bits" heuristic for unknown layouts?
2. Trit-space (Princeton 0/1/float) in v1, or binary-only first and add trits in
   a follow-up?
