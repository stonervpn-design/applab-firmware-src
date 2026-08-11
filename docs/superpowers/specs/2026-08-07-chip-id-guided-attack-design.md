# P1: Capture → Encoder-Chip ID → Guided Attack (design)

**Date:** 2026-08-07
**Status:** approved (design), pending plan
**Scope:** PRIVATE builds only. This is the first roadmap item toward matching/exceeding the PandwaRF Rogue Pro; it delivers PandwaRF's signature "capture → identify the encoder chip → one-tap attack" flow, decoded-first.

---

## Goal

When the user opens a captured **fixed-code** sub-GHz signal, the Signal-ID screen names the **encoder chip** (Princeton/PT2262/EV1527, CAME, NICE, Holtek, Ansonic, Chamberlain, Linear…) and offers a one-press **[Attack]** button that drops the user into the SubBrute framed brute-forcer **already pre-configured** with the matching preset for that chip family, bit-width, and frequency.

## Non-goals (explicitly deferred)

- **Fixed-bit masking** (sweep only the variable/data bits) → **P2**, together with the fast-brute engine. P1 hands off to the *existing* framed brute.
- **Fast-brute engine** (kill the per-code delay, pipeline TX) → **P2**.
- **Raw / undecoded chip guessing** (identify a signal no decoder matched, from ProtoView timing) → later fast-follow. P1 is **decoded-first**: it keys off the authoritative decoded protocol name.
- Any change to public/webflasher builds.

---

## GLOBAL CONSTRAINT — PRIVATE BUILD ONLY (non-negotiable)

Every part of this feature must be absent from public/webflasher builds:

1. The new source file's entire body is wrapped in `#ifndef PUBLIC_BUILD … #endif` (compiles to nothing in public).
2. Every call site added to `subghz_scene_analyze.c` (chip line render, [Attack] button, event handling, handoff) is wrapped in `#ifndef PUBLIC_BUILD`.
3. Result: gc-sections drops the code from public even if the object is linked; **verification gate** below must pass before any public deploy.
4. **Verification gate (must pass):** on the public ELF, `nm` shows **0** occurrences of `subghz_chip_identify` (and any other new symbol); `grep` of the merged public bin finds **none** of the new chip-label strings (e.g. "PT2262", "Princeton-class"); `tools/verify_public_no_car.py` still PASSES. Compare against the private ELF where the symbols ARE present. This mirrors how the RF-analyzer arc was verified.

---

## Architecture

Two layers, keeping the classifier pure and host-testable:

1. **Pure classifier** — `helpers/subghz_chip_id.c/.h`: maps a decoded protocol name (+ bit count, freq, modulation) to a **chip family + human label**. No radio, no storage, no UI, no SubBrute dependency. Host-testable with a Python replica (same discipline as `subghz_mod_classify` / `signal_fingerprint`).
2. **Scene integration + handoff** — `subghz_scene_analyze.c`: for fixed-code captures, runs the classifier, renders the chip line, and (when a matching SubBrute preset exists) shows an **[Attack]** spoke that pre-selects the attack and navigates into SubBrute. The `(family, bits, freq) → SubBruteAttacks` resolution is a small on-device static function local to the scene (it may include `subbrute_protocols.h`; the *core* classification stays dependency-free).

### Data flow

```
history capture (decoded, fixed-code)
   -> subghz_scene_analyze_on_enter
      -> subghz_chip_identify(proto_name, bits, freq, mod)  [PURE]
           -> SubGhzChipId { chip_label, family, total_bits, fixed }
      -> render "Chip: <label> · <bits>-bit · Fixed"
      -> chip_family_to_subbrute_attack(family, bits, freq)  [scene-local]
           -> SubBruteAttacks value, or -1
      -> if >=0: add [Attack] button
[Attack] pressed
   -> subbrute_device_attack_set(subghz->subbrute_device, attack)
   -> scene_manager_next_scene(SubGhzSceneBfSetupAttack)   (user still confirms TX)
```

---

## Components & interfaces

### NEW `applications/main/subghz/helpers/subghz_chip_id.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "signal_fingerprint.h" // SignalMod

typedef enum {
    SubGhzChipUnknown = 0,
    SubGhzChipPrinceton,   // PT2262/PT2260/EV1527/SMC5326 class, 24-bit PWM
    SubGhzChipCame,        // CAME 12-bit
    SubGhzChipNice,        // NICE FLO 12-bit
    SubGhzChipHoltek,      // HT12/HT6 12-bit
    SubGhzChipAnsonic,     // 12-bit
    SubGhzChipChamberlain, // 7/8/9-bit
    SubGhzChipLinear,      // 10-bit
    SubGhzChipLinearDelta, // 8-bit
} SubGhzChipFamily;

typedef struct {
    const char* chip_label; // human string, e.g. "Princeton-class (PT2262/EV1527)"
    SubGhzChipFamily family;
    int32_t total_bits;
    bool fixed;             // true = fixed-code (attackable via framed brute)
} SubGhzChipId;

// Pure. Maps a DECODED protocol name to an encoder-chip identity. Returns
// family=SubGhzChipUnknown for unrecognized / rolling protocols. No radio/UI.
SubGhzChipId subghz_chip_identify(
    const char* proto_name, int32_t bits, uint32_t freq_hz, SignalMod mod);
```

Entire `.c` body guarded by `#ifndef PUBLIC_BUILD`.

### NEW `tests/test_subghz_chip_id.c`

Host test (user CI; no gcc locally → validated via Python replica + xtensa `-fsyntax-only`). Cases: each family's decoded name → expected family + label; unknown name → SubGhzChipUnknown; a rolling name (e.g. "KeeLoq") → SubGhzChipUnknown.

### MODIFY `applications/main/subghz/scenes/subghz_scene_analyze.c`

- In the fixed-code branch (the current "Not a known automotive fob / generic device" path), call `subghz_chip_identify` and, when `family != SubGhzChipUnknown`, render `Chip: <chip_label>  <bits>-bit  Fixed`.
- Add scene-local `static int chip_family_to_subbrute_attack(SubGhzChipFamily, int32_t bits, uint32_t freq)` returning a `SubBruteAttacks` value or -1 (nearest-frequency variant; -1 when none).
- When the resolver returns >=0, add a `[Attack]` button (distinct from the existing Left=Details spoke used for rolling/PSA — use `GuiButtonTypeRight` / a new custom event).
- All additions wrapped in `#ifndef PUBLIC_BUILD`.

### MODIFY `applications/main/subghz/helpers/subghz_custom_event.h`

- Add `SubGhzCustomEventSceneAnalyzeAttack` (guarded if the enum block permits, else the value is inert in public since nothing emits it).

### Decoded-protocol → chip → attack mapping (resolver table)

| Decoded name (`strstr`) | Chip label | Family | SubBrute target (nearest freq) |
|---|---|---|---|
| "Princeton" | Princeton-class (PT2262/EV1527) | Princeton | PT2260/SMC5326/UNILARM 24-bit @ 315/330/390/433 |
| "CAME" | CAME 12-bit | Came | CAME 12-bit @ 303/307/315/330/433/868 |
| "Nice" | NICE 12-bit | Nice | NICE 12-bit @ 433/868 |
| "Holtek" | Holtek HT12/HT6 12-bit | Holtek | Holtek 12-bit AM/FM @ 315/433/868/915 |
| "Ansonic" | Ansonic 12-bit | Ansonic | Ansonic 12-bit @ 433.075/433/434 |
| "Chamberlain" | Chamberlain %d-bit | Chamberlain | Chamberlain 7/8/9-bit @ 300/315/318/390/433 (by bits) |
| "Linear" (+"Delta") | Linear / Linear Delta | Linear/LinearDelta | Linear 10-bit @ 300/310 / LinearDelta 8-bit @ 310 |

Nearest-frequency selection: pick the preset whose frequency is closest to the capture's frequency; if the family/bits/freq combination has no preset, return -1 → no [Attack] button (never a dead-end).

---

## Safety

- The analyze scene remains **read-only**. `[Attack]` only *navigates* to SubBrute; the actual transmit still requires the user's explicit confirm inside SubBrute. No auto-TX is introduced.
- Authorized-use posture unchanged: this is on the user's own private research firmware for their own/authorized devices; the whole feature is private-build-gated.
- A wrong chip label is a mislabel, not an RF action; the worst case is the user lands in a mismatched (still user-confirmed) brute preset.

---

## Testing & verification

1. **Host (pure core):** `test_subghz_chip_id.c` — Python replica cross-check + `xtensa -fsyntax-only` (no local gcc).
2. **Private build:** compiles green; `nm` on the private ELF shows `subghz_chip_identify` PRESENT; on-device smoke: open a captured CAME/Princeton signal → chip line + [Attack] → lands in the correct pre-selected SubBrute preset.
3. **Public build (the gate):** compiles green; `nm` on the public ELF shows `subghz_chip_identify` = **0**; merged public bin `grep` finds none of the new chip-label strings; `verify_public_no_car.py` PASSES.

---

## Open items for the plan

- Confirm the exact `SubBruteAttacks` enum spellings when writing the resolver (from `subbrute_protocols.h`).
- Confirm whether `subbrute_device_attack_set` needs a following `_set_default_values` call for a clean pre-select.
- Decide button layout when a capture is BOTH decodable and has a Details spoke (fixed-code won't be dynamic, so Details vs Attack are mutually exclusive in practice).
