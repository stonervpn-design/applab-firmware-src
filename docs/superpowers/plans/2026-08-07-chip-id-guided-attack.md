# P1: Chip-ID → Guided Attack — Implementation Plan

> **For agentic workers:** implement task-by-task. Steps use `- [ ]` checkboxes. Private builds only.

**Goal:** For a decoded fixed-code capture, name the encoder chip on the Signal-ID screen and offer a one-press [Attack] that opens SubBrute pre-configured with the matching framed preset.

**Architecture:** A pure, host-tested classifier (`subghz_chip_id`) maps a decoded protocol name → chip family + label; `subghz_scene_analyze.c` renders it and pre-selects the SubBrute attack on handoff. Decoded-first (keys off the authoritative decoded name).

**Tech Stack:** ESP-IDF v5.4.1, C, the existing subghz app + SubBrute helpers.

## Global Constraints

- **PRIVATE BUILD ONLY.** New `.c` body fully wrapped in `#ifndef PUBLIC_BUILD … #endif`; every analyze-scene addition wrapped in `#ifndef PUBLIC_BUILD`. Public builds must contain **zero** of this feature (verified by nm + bin grep in Task 3).
- Read-only analyze scene: `[Attack]` only navigates; TX still user-confirmed in SubBrute. No auto-TX.
- No local gcc → validate pure logic via Python replica + `xtensa-esp32s3-elf-gcc -fsyntax-only`.
- Build one env at a time. Private: `build_m5stick_s3.bat`. Public gate: `build_m5stick_s3_public.bat`.
- nm: `C:\Users\stone\.espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe`.

---

### Task 1: Pure chip-ID classifier + host test

**Files:**
- Create: `applications/main/subghz/helpers/subghz_chip_id.h`
- Create: `applications/main/subghz/helpers/subghz_chip_id.c`
- Test: `tests/test_subghz_chip_id.c`
- Create: `tools/chip_id/chip_id_replica.py` (validation replica)

**Interfaces produced:** `SubGhzChipFamily`, `SubGhzChipId`, `SubGhzChipId subghz_chip_identify(const char* proto_name, int32_t bits, uint32_t freq_hz, SignalMod mod)` (per spec header block).

- [ ] **Step 1 — Header.** Write `subghz_chip_id.h` exactly as the spec's header block (enum `SubGhzChipFamily`, struct `SubGhzChipId`, `subghz_chip_identify` decl; includes `signal_fingerprint.h` for `SignalMod`).

- [ ] **Step 2 — Implementation (private-guarded).** Write `subghz_chip_id.c`:
```c
#include "subghz_chip_id.h"
#ifndef PUBLIC_BUILD
#include <string.h>
static bool has(const char* h, const char* n){ return h && strstr(h, n); }
SubGhzChipId subghz_chip_identify(const char* proto_name, int32_t bits, uint32_t freq_hz, SignalMod mod){
    (void)freq_hz; (void)mod;
    SubGhzChipId r = { "", SubGhzChipUnknown, bits, false };
    if(has(proto_name, "Princeton")) { r.family=SubGhzChipPrinceton; r.chip_label="Princeton-class (PT2262/EV1527)"; r.fixed=true; }
    else if(has(proto_name, "CAME"))        { r.family=SubGhzChipCame;   r.chip_label="CAME 12-bit"; r.fixed=true; }
    else if(has(proto_name, "Nice"))        { r.family=SubGhzChipNice;   r.chip_label="NICE 12-bit"; r.fixed=true; }
    else if(has(proto_name, "Holtek"))      { r.family=SubGhzChipHoltek; r.chip_label="Holtek HT12/HT6 12-bit"; r.fixed=true; }
    else if(has(proto_name, "Ansonic"))     { r.family=SubGhzChipAnsonic;r.chip_label="Ansonic 12-bit"; r.fixed=true; }
    else if(has(proto_name, "Chamberlain")) { r.family=SubGhzChipChamberlain; r.chip_label="Chamberlain"; r.fixed=true; }
    else if(has(proto_name, "Linear") && has(proto_name, "Delta")) { r.family=SubGhzChipLinearDelta; r.chip_label="Linear Delta 8-bit"; r.fixed=true; }
    else if(has(proto_name, "Linear"))      { r.family=SubGhzChipLinear;r.chip_label="Linear 10-bit"; r.fixed=true; }
    return r; // Unknown for rolling/unrecognized (KeeLoq, Kia, etc.)
}
#endif
```

- [ ] **Step 3 — Host test.** Write `tests/test_subghz_chip_id.c` including `../applications/main/subghz/helpers/subghz_chip_id.c`; assert: "Princeton"→SubGhzChipPrinceton & fixed; "CAME"→Came; "Nice FLO"→Nice; "Holtek HT12"→Holtek; "Ansonic"→Ansonic; "Chamberlain"→Chamberlain; "Linear Delta"→LinearDelta; "Linear"→Linear; "KeeLoq"→SubGhzChipUnknown & !fixed; NULL→Unknown. (Guard note: define `PUBLIC_BUILD`-absent for the test build.)

- [ ] **Step 4 — Python replica + validate.** Write `tools/chip_id/chip_id_replica.py` mirroring the mapping; run the same cases; confirm parity. Then `xtensa-esp32s3-elf-gcc -fsyntax-only -I… subghz_chip_id.c` clean.

- [ ] **Step 5 — Commit.** `feat(subghz): pure encoder-chip classifier + host test (private-only)`

---

### Task 2: Analyze-scene chip line + [Attack] handoff

**Files:**
- Modify: `applications/main/subghz/helpers/subghz_custom_event.h` (add `SubGhzCustomEventSceneAnalyzeAttack`)
- Modify: `applications/main/subghz/scenes/subghz_scene_analyze.c`

**Interfaces consumed:** `subghz_chip_identify` (Task 1); `subbrute_device_attack_set` / `subghz->subbrute_device->attack` (subbrute_device.h); `SubBruteAttacks` (subbrute_protocols.h); `SubGhzSceneBfSetupAttack`.

- [ ] **Step 1 — Event.** Add `SubGhzCustomEventSceneAnalyzeAttack` to the custom-event enum.

- [ ] **Step 2 — Resolver.** In `subghz_scene_analyze.c`, add a `#ifndef PUBLIC_BUILD` scene-local `static int chip_family_to_subbrute_attack(SubGhzChipFamily f, int32_t bits, uint32_t freq)` returning the nearest-frequency `SubBruteAttacks` value per the spec table, or `-1`. Use exact enum spellings from `subbrute_protocols.h` (verify at write time).

- [ ] **Step 3 — Render + button.** In the fixed-code branch (the current "generic device" path), under `#ifndef PUBLIC_BUILD`: call `subghz_chip_identify`; if `family != SubGhzChipUnknown`, append `Chip: <chip_label>  <bits>-bit  Fixed` to the body; compute the resolver; if `>=0`, add a `[Attack]` button (`GuiButtonTypeRight`) whose callback sends `SubGhzCustomEventSceneAnalyzeAttack`.

- [ ] **Step 4 — Handle event.** In `subghz_scene_analyze_on_event`, under `#ifndef PUBLIC_BUILD`: on `SubGhzCustomEventSceneAnalyzeAttack`, call `subbrute_device_attack_set(subghz->subbrute_device, attack)` (+ `_set_default_values` if required), then `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfSetupAttack)`. Stash the resolved attack in a static/scene field between enter and event.

- [ ] **Step 5 — Build private + verify symbol.** `build_m5stick_s3.bat` → SENTINEL_OK; `nm build_m5stick_s3/furi_esp32.elf | grep subghz_chip_identify` → PRESENT.

- [ ] **Step 6 — Commit.** `feat(subghz): chip-ID line + guided [Attack] handoff in Signal-ID (private-only)`

---

### Task 3: Public-build exclusion gate

**Files:** none (verification only).

- [ ] **Step 1 — Build public.** `build_m5stick_s3_public.bat` → SENTINEL_OK.
- [ ] **Step 2 — Symbol gate.** `nm build_m5stick_s3_public/furi_esp32.elf | grep -c subghz_chip_identify` → **0**.
- [ ] **Step 3 — String gate.** `grep -a` the merged public bin for `"Princeton-class"`, `"CAME 12-bit"`, `"Chip:"` → **0** hits.
- [ ] **Step 4 — Car gate unaffected.** `python tools/verify_public_no_car.py Flipper-m5stick_s3-public-merged.bin` → PASS.
- [ ] **Step 5 — Commit.** `chore(subghz): verify chip-ID feature is private-only (public gate clean)`

---

## Self-review

- **Coverage:** Task 1 = pure classifier (spec §Components), Task 2 = scene integration + handoff (spec §Architecture/§Components), Task 3 = the PRIVATE-ONLY gate (spec §Global constraint / §Verification). All spec sections mapped.
- **Type consistency:** `SubGhzChipFamily`/`SubGhzChipId`/`subghz_chip_identify` used identically across tasks and the spec.
- **No placeholders:** mapping data + interfaces are concrete; the only write-time lookups are exact `SubBruteAttacks` spellings (flagged) and whether `_set_default_values` is needed (flagged).
