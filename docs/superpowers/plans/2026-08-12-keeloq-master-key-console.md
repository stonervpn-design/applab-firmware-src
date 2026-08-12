# KeeLoq Master-Key Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make all 216 plaintext KeeLoq keys usable end-to-end (identify → generate/emulate → clone/predict) from one console, backed by a single routing registry, proven by a host round-trip matrix.

**Architecture:** A new pure `subghz_keeloq_registry` maps every keystore entry (by learning type) to `{family, generator, encodable, decode_only}`. A catalog console scene lists all entries and routes each to its correct existing generator; the Signal-ID hub gains registry naming + an explicit known-key dictionary attack. No working protocol code is rewritten.

**Tech Stack:** C (ESP32-S3 / ESP-IDF v5.4.1), Flipper-port subghz app; host unit/coverage tests via WSL gcc (`tools/decoder_host_test`). Spec: `docs/superpowers/specs/2026-08-12-keeloq-master-key-console-design.md`.

## Global Constraints

- **PRIVATE build only — hard rule.** Every KeeLoq master key, car decoder, clone,
  keystore asset, and all new console/attack UI in this plan is PRIVATE and stays
  OFF the public build and the public webflasher. Public inclusion requires the
  owner's explicit password authorization, which is **NOT granted** for this work.
  Nothing here is authorized for public. All new UI is guarded `#ifndef PUBLIC_BUILD`.
- **The public build is only ever compiled here to PROVE exclusion — never to ship
  this content.** `build_m5stick_s3_public.bat` runs the automated no-car guard
  (`SENTINEL_VERIFY_NOCAR` → `PASS: no car material ...`). A public build in this
  plan means "confirm the public bin still EXCLUDES all of this," full stop. If the
  no-car guard ever reports anything other than PASS, treat it as a leak and STOP.
- Build via the **PowerShell tool** running the `.bat` scripts (not Bash):
  `build_m5stick_s3.bat` (private — where this feature lives),
  `build_adv_only_private.bat` (private cardputer),
  `build_m5stick_s3_public.bat` (public — **exclusion check only**). Success =
  `SENTINEL_OK` in the matching `.log`; a PowerShell exit-1 with `SENTINEL_OK`
  present is the known native-stderr artifact, not a failure. **Clean-reconfigure is
  automatic** in these scripts; when adding a new `.c`, the `reconfigure` step picks
  it up (CMake globs `applications/main/subghz`).
- Host tests run the ELF **inside WSL**: `wsl.exe -e bash -lc '...'`.
- No new radio/TX stack. Generate/emulate reuse the existing `Gen*` byte-input flow and the standard txrx encoder path.
- Keystore entry type is `SubGhzKey { FuriString* name; uint64_t key; uint16_t type; }`, iterated with `M_EACH(k, *subghz_keystore_get_data(keystore), SubGhzKeyArray_t)`.
- Learning-type constants (`lib/subghz/protocols/keeloq_common.h`): UNKNOWN 0, SIMPLE 1, NORMAL 2, SECURE 3, MAGIC_XOR_TYPE_1 4, FAAC 5, MAGIC_SERIAL_TYPE_1 6, MAGIC_SERIAL_TYPE_2 7, MAGIC_SERIAL_TYPE_3 8, SIMPLE_KINGGATES 10, NORMAL_JAROLIFT 11, ERREKA 12, PUJOL 13, AERF 14, SIMPLE_JCM 15. (Type 9 is undefined in the enum — 3 keystore rows use it; see Task 1.)
- Generators (`helpers/subghz_gen_info.h`): `GenKeeloq`, `GenKeeloqBFT`, `GenFaacSLH`, `GenKingGatesStylo4k`, `GenJarolift`.
- Commit message trailer on every commit:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01Kk6c15xdcA4LAim6BJnEtv`

**Approved open-item defaults (from spec):**
1. Type-9: `KIA_Hyundai_V5` → Kia V5 (capture/car-fob emulate path, decode-only). Beninca: **FINAL behavior (refined during Task 4 review, supersedes the draft below)** — only **type-9** `Beninca_ARC` is encodable via `GenBenincaARC`; **type-10/11 Beninca** (e.g. `BenincaVA`) is **decode-only**, because `beninca_arc.c`'s key lookup only matches type-9 keys (a type-10/11 entry would emulate with the wrong key). A bare type-9 that is neither KIA nor Beninca is a true orphan (decode-only). (The Task-1 code blocks below show the earlier draft where all Beninca types were encodable; the shipped registry + `test_keeloq_registry.c` use the final type-9-only rule.)
2. Console lives under the **Car Keys** hub (`subghz_scene_car_keys.c`).
3. Type-0 generics (25) ship **decode/attack-only in v1** (no choose-mode generate yet).

---

### Task 1: KeeLoq Master-Key Registry + host unit test

**Files:**
- Create: `applications/main/subghz/helpers/subghz_gen_type.h` (extract `GenType` enum — Step 0)
- Modify: `applications/main/subghz/helpers/subghz_gen_info.h` (drop inline `GenType`, include the new leaf header)
- Create: `applications/main/subghz/helpers/subghz_keeloq_registry.h`
- Create: `applications/main/subghz/helpers/subghz_keeloq_registry.c`
- Test: `tests/test_keeloq_registry.c`

**Interfaces:**
- Consumes: `GenType` — but via a NEW dependency-free leaf header `subghz_gen_type.h` (extracted in Step 0), NOT `subghz_gen_info.h` (which pulls `subghz_types.h` → `<furi.h>` → mlib and will NOT host-compile). The registry switches on **literal** learning-type numbers (0–15, per Global Constraints) with symbolic names in comments, so it does NOT include `keeloq_common.h` (also furi-coupled). Net: the registry + its test compile with `-Iapplications/main/subghz/helpers` and nothing else.
- Produces (later tasks depend on these EXACT names):
  - `typedef enum { KlFamGeneric, KlFamFaacSLH, KlFamKingGates, KlFamJarolift, KlFamBeninca, KlFamKiaCar, KlFamOrphan } KeeloqFamily;`
  - `typedef struct { KeeloqFamily family; GenType generator; bool encodable; bool decode_only; } KeeloqTypeInfo;`
  - `KeeloqTypeInfo subghz_keeloq_registry_type_info(uint16_t type, const char* name);`
  - `bool subghz_keeloq_registry_type_encodable(uint16_t type);`  // convenience: type_info(type,NULL).encodable

**Design of `type_info` (the routing table):**

| type | family | generator | encodable | decode_only |
|---|---|---|---|---|
| 1,2,3,4,6,7,8,12,13,14,15 | Generic | `GenKeeloq` | true | false |
| 0 (UNKNOWN) | Generic | `GenKeeloq` | false | true |
| 5 (FAAC) | FaacSLH | `GenFaacSLH` | true | false |
| 10 (KingGates) | KingGates | `GenKingGatesStylo4k` | true | false |
| 11 (Jarolift) | Jarolift | `GenJarolift` | true | false |
| 9 | Kia / Beninca / Orphan by name | `GenBenincaARC` for Beninca | Beninca: true; else false | Kia+orphan: true |

Name disambiguation (applied only for types 9/10/11, checked in this order): (1) name starts with `"KIA"` → `KlFamKiaCar`, `encodable=false`, `decode_only=true` (v1: capture/car-fob path, not catalog generate); (2) name contains `"Beninca"` → `KlFamBeninca`, generator `GenBenincaARC`, `encodable=true` (Beninca_ARC/BenincaVA appear under types 9 and 11 but have a real `GenBenincaARC` route); (3) otherwise type 10 → KingGates, type 11 → Jarolift; a bare type 9 that is neither KIA nor Beninca → `KlFamOrphan`, `decode_only=true`.

- [ ] **Step 0: Extract `GenType` into a leaf header (so the registry is furi-free)**

Create `applications/main/subghz/helpers/subghz_gen_type.h` containing ONLY the
`GenType` enum, copied verbatim from `subghz_gen_info.h` (all 16 members in order:
`GenData, GenFaacSLH, GenKeeloq, GenCameAtomo, GenKeeloqBFT, GenAlutechAt4n, GenSomfyTelis, GenSomfyKeytis, GenKingGatesStylo4k, GenBenincaARC, GenJarolift, GenDitecGOL4, GenNiceFlorS, GenSecPlus1, GenSecPlus2, GenPhoenixV2`):

```c
#pragma once
// GenType lives here (leaf, no includes) so pure/host-testable helpers can use it
// without dragging in subghz_types.h -> <furi.h>. subghz_gen_info.h includes this.
typedef enum {
    GenData, GenFaacSLH, GenKeeloq, GenCameAtomo, GenKeeloqBFT, GenAlutechAt4n,
    GenSomfyTelis, GenSomfyKeytis, GenKingGatesStylo4k, GenBenincaARC, GenJarolift,
    GenDitecGOL4, GenNiceFlorS, GenSecPlus1, GenSecPlus2, GenPhoenixV2,
} GenType;
```

Then edit `subghz_gen_info.h`: DELETE its inline `typedef enum {...} GenType;` and add `#include "subghz_gen_type.h"` after its existing includes. (Firmware behavior unchanged — same enum, same values, just relocated.)

- [ ] **Step 1: Write the registry header**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "subghz_gen_type.h" // GenType (furi-free leaf; NOT subghz_gen_info.h)

typedef enum {
    KlFamGeneric,
    KlFamFaacSLH,
    KlFamKingGates,
    KlFamJarolift,
    KlFamBeninca,
    KlFamKiaCar,
    KlFamOrphan,
} KeeloqFamily;

typedef struct {
    KeeloqFamily family;
    GenType generator; // valid only when encodable
    bool encodable;    // has a working generate/emulate route from the catalog
    bool decode_only;  // reachable by decode/attack but not catalog-generate (v1)
} KeeloqTypeInfo;

// Classify a keystore entry by (learning type, manufacturer name). `name` may be
// NULL when only the type matters; name is consulted only for the shared
// types 9/10/11 (Kia vs dedicated protocol). Never returns an invalid generator
// when encodable is true.
KeeloqTypeInfo subghz_keeloq_registry_type_info(uint16_t type, const char* name);

// Convenience gate used by the by-manufacturer picker (replaces its local list).
bool subghz_keeloq_registry_type_encodable(uint16_t type);
```

- [ ] **Step 2: Write the failing test** (`tests/test_keeloq_registry.c`)

```c
#include "../applications/main/subghz/helpers/subghz_keeloq_registry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    // Generic encodable types
    uint16_t generic[] = {1,2,3,4,6,7,8,12,13,14,15};
    for(unsigned i = 0; i < sizeof(generic)/sizeof(generic[0]); i++) {
        KeeloqTypeInfo t = subghz_keeloq_registry_type_info(generic[i], NULL);
        assert(t.family == KlFamGeneric);
        assert(t.encodable && !t.decode_only);
        assert(t.generator == GenKeeloq);
    }
    // The four formerly-HIDDEN types are now encodable (the +36-key bug)
    uint16_t hidden[] = {12,13,14,15};
    for(unsigned i = 0; i < 4; i++)
        assert(subghz_keeloq_registry_type_encodable(hidden[i]) == true);

    // type-0 generics: decode-only in v1
    KeeloqTypeInfo t0 = subghz_keeloq_registry_type_info(0, NULL);
    assert(!t0.encodable && t0.decode_only && t0.family == KlFamGeneric);

    // Dedicated protocols
    assert(subghz_keeloq_registry_type_info(5, "FAAC_SLH").family == KlFamFaacSLH);
    assert(subghz_keeloq_registry_type_info(5, "FAAC_SLH").generator == GenFaacSLH);
    assert(subghz_keeloq_registry_type_info(10, "Kingates_Stylo4k").family == KlFamKingGates);
    assert(subghz_keeloq_registry_type_info(11, "Jarolift").family == KlFamJarolift);

    // Name disambiguation for shared types
    assert(subghz_keeloq_registry_type_info(10, "KIA_Hyundai_V3&4").family == KlFamKiaCar);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").family == KlFamKiaCar);
    // Beninca appears under types 9/11 but has a real GenBenincaARC route
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").family == KlFamBeninca);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").encodable);
    assert(subghz_keeloq_registry_type_info(9, "Beninca_ARC").generator == GenBenincaARC);
    // Kia is decode-only (capture/car path), not catalog-encodable
    assert(!subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").encodable);
    assert(subghz_keeloq_registry_type_info(9, "KIA_Hyundai_V5").decode_only);

    printf("test_keeloq_registry: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run it, verify it fails to link**

Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Iapplications/main/subghz/helpers tests/test_keeloq_registry.c applications/main/subghz/helpers/subghz_keeloq_registry.c -o /tmp/tkr 2>&1 | tail'`
Expected: link/compile FAIL (no `subghz_keeloq_registry.c` yet, or undefined symbols).

- [ ] **Step 4: Implement `subghz_keeloq_registry.c`**

```c
#include "subghz_keeloq_registry.h"
#include <string.h>

// Learning-type numbers are the keystore's on-disk `type` field (a wire value),
// not KeeLoq crypto, so we switch on literals (symbolic names in comments, per
// Global Constraints) and stay furi-free / host-testable. Values:
// 0 UNKNOWN,1 SIMPLE,2 NORMAL,3 SECURE,4 MAGIC_XOR_1,5 FAAC,6 MAGIC_SERIAL_1,
// 7 MAGIC_SERIAL_2,8 MAGIC_SERIAL_3,10 SIMPLE_KINGGATES,11 NORMAL_JAROLIFT,
// 12 ERREKA,13 PUJOL,14 AERF,15 SIMPLE_JCM (9 is undefined in the enum).

static bool name_is_kia(const char* name) {
    return name && (strncmp(name, "KIA", 3) == 0);
}

static bool name_has_beninca(const char* name) {
    return name && (strstr(name, "Beninca") != NULL);
}

KeeloqTypeInfo subghz_keeloq_registry_type_info(uint16_t type, const char* name) {
    KeeloqTypeInfo r = {KlFamGeneric, GenKeeloq, false, false};
    switch(type) {
    case 1:  // SIMPLE
    case 2:  // NORMAL
    case 3:  // SECURE
    case 4:  // MAGIC_XOR_TYPE_1
    case 6:  // MAGIC_SERIAL_TYPE_1
    case 7:  // MAGIC_SERIAL_TYPE_2
    case 8:  // MAGIC_SERIAL_TYPE_3
    case 12: // ERREKA
    case 13: // PUJOL
    case 14: // AERF
    case 15: // SIMPLE_JCM
        r.family = KlFamGeneric; r.generator = GenKeeloq; r.encodable = true;
        return r;
    case 0:  // UNKNOWN — generic dictionary key, v1 decode-only
        r.family = KlFamGeneric; r.decode_only = true; return r;
    case 5:  // FAAC
        r.family = KlFamFaacSLH; r.generator = GenFaacSLH; r.encodable = true; return r;
    case 10: // SIMPLE_KINGGATES (or KIA/Beninca by name)
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.generator = GenBenincaARC; r.encodable = true; return r; }
        r.family = KlFamKingGates; r.generator = GenKingGatesStylo4k; r.encodable = true; return r;
    case 11: // NORMAL_JAROLIFT (or KIA/Beninca by name)
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.generator = GenBenincaARC; r.encodable = true; return r; }
        r.family = KlFamJarolift; r.generator = GenJarolift; r.encodable = true; return r;
    case 9:  // undefined enum slot; KIA_Hyundai_V5 (car), Beninca_ARC (GenBenincaARC), else orphan
        if(name_is_kia(name)) { r.family = KlFamKiaCar; r.decode_only = true; return r; }
        if(name_has_beninca(name)) { r.family = KlFamBeninca; r.generator = GenBenincaARC; r.encodable = true; return r; }
        r.family = KlFamOrphan; r.decode_only = true; return r;
    default:
        r.family = KlFamOrphan; r.decode_only = true; return r;
    }
}

bool subghz_keeloq_registry_type_encodable(uint16_t type) {
    return subghz_keeloq_registry_type_info(type, NULL).encodable;
}
```

- [ ] **Step 5: Run the test, verify PASS**

Run: same gcc command as Step 3, then `wsl.exe -e bash -lc '/tmp/tkr'`
Expected: `test_keeloq_registry: ALL PASS`

- [ ] **Step 6: Commit**

```bash
git add applications/main/subghz/helpers/subghz_keeloq_registry.c applications/main/subghz/helpers/subghz_keeloq_registry.h tests/test_keeloq_registry.c
git commit  # subject: "keeloq-console Task 1: master-key routing registry + host test"
```

---

### Task 2: Host coverage matrix (generic KeeLoq round-trip)

**Files:**
- Create: `tools/decoder_host_test/keeloq_coverage.c`
- Create: `tools/decoder_host_test/keeloq_coverage_build.sh`
- Reference: existing `tools/decoder_host_test/build_lib.sh` for the compile pattern and shim includes.

**Interfaces:**
- Consumes: `subghz_keeloq_registry_type_info`; the real `keeloq.c` encoder+decoder; `keystore_host.c` (already loads the 216 keys for the host harness).
- Produces: a printed 216-row matrix (`PASS` / `DECODE-ONLY` / `ORPHANED` / `FAIL <reason>`) and a `SUMMARY` line; exit non-zero if any `encodable` generic row is not `PASS`.

- [ ] **Step 1: Write `keeloq_coverage.c`**

For each keystore entry: classify via `subghz_keeloq_registry_type_info(type,name)`.
- `encodable && family==KlFamGeneric`: seed a FlipperFormat with Protocol="KeeLoq", the entry's serial-derived fields, `Btn`, `Cnt=N`; run the KeeLoq encoder → durations → decoder; assert the decoded manufacture name matches and `Cnt` advances N→N+1. Print `PASS`/`FAIL`.
- `encodable && family∈{FaacSLH,KingGates,Jarolift}`: classify `ROUTE:<family>` (dedicated encoder round-trip is out of this harness's v1 scope — its own protocol tests cover it) — print `ROUTE-OK` (not a failure).
- `decode_only`: print `DECODE-ONLY`.
- `KlFamOrphan`: print `ORPHANED`.

Follow the exact FlipperFormat seeding pattern already used in `tools/decoder_host_test/fob_sweep.c` for KeeLoq (reuse its helper for building a KeeLoq `.sub`-format frame).

- [ ] **Step 2: Write the build script** (`keeloq_coverage_build.sh`) — copy `build_lib.sh`, swap the target `.c` to `keeloq_coverage.c` and the output to `keeloq_coverage`, keep the same `-I` include set and the keeloq/keystore source list.

- [ ] **Step 3: Build + run, verify the matrix**

Run: `wsl.exe -e bash -lc 'cd "/mnt/c/.../tools/decoder_host_test" && bash keeloq_coverage_build.sh && ./keeloq_coverage | tail -40'`
Expected: every generic encodable row `PASS` (must include ≥1 each of types 12,13,14,15 — the un-hidden set); `SUMMARY` shows 0 generic FAIL; dedicated rows (FAAC/KingGates/Jarolift/Beninca) `ROUTE-OK`; type-0 `DECODE-ONLY`; KIA type-9 `DECODE-ONLY`; only a genuinely-unrouted type row (if any) `ORPHANED`.

- [ ] **Step 4: Commit**

```bash
git add tools/decoder_host_test/keeloq_coverage.c tools/decoder_host_test/keeloq_coverage_build.sh
git commit  # subject: "keeloq-console Task 2: host coverage matrix for the 216 keys"
```

---

### Task 3: Un-hide the 36 encoder-ready keys (picker uses the registry)

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_keeloq_mfr.c` (delete local `subghz_scene_keeloq_mfr_type_encodable`, call the registry).

**Interfaces:**
- Consumes: `subghz_keeloq_registry_type_encodable` (Task 1).

- [ ] **Step 1: Replace the local allow-list**

Add include `#include "../helpers/subghz_keeloq_registry.h"`. Delete the whole `static bool subghz_scene_keeloq_mfr_type_encodable(uint16_t type){...}` function. Replace both call sites (`on_enter` count loop and `submenu_callback` index loop) with `subghz_keeloq_registry_type_encodable(key->type)`.

- [ ] **Step 2: Build private m5stick, verify SENTINEL_OK**

PowerShell: `& cmd.exe /c build_m5stick_s3.bat` → check `build_m5stick_s3.log` ends `SENTINEL_OK`.

- [ ] **Step 3: Confirm the count grew by 36**

The picker's encodable count = generic-encodable entries. Re-run the Task-2 coverage matrix and confirm the encodable-generic count is 179 (was 143). (Manual on-device: "KL: By Manufacturer" now lists ERREKA/PUJOL/AERF/JCM manufacturers.)

- [ ] **Step 4: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_keeloq_mfr.c
git commit  # subject: "keeloq-console Task 3: un-hide 36 encoder-ready keys via registry"
```

---

### Task 4: Catalog console scene (browse all → route to generator)

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_keeloq_console.c`
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (add `ADD_SCENE(subghz, keeloq_console, KeeloqConsole)` in the PRIVATE-gated block near `keeloq_mfr`)
- Modify: `applications/main/subghz/scenes/subghz_scene_car_keys.c` (add a "KeeLoq Console (all keys)" submenu row that enters `SubGhzSceneKeeloqConsole`)

**Interfaces:**
- Consumes: `subghz_keeloq_registry_type_info` (Task 1); the existing `gen_info->keeloq.manuf` + `keeloq_mfr_emulate_flow` handoff and `SubGhzSceneSetSerial`; `gen_info` `GenType` selection used by `subghz_scene_set_serial.c`.
- Produces: `SubGhzSceneKeeloqConsole` scene.

**Behavior:** mirror `subghz_scene_keeloq_mfr.c`, but (a) iterate ALL entries whose `type_info(type,name).encodable` is true (across families), (b) on select, set `gen_info->keeloq.manuf` (stable copy, as the mfr scene does) AND set the generator for the SetSerial flow to `type_info.generator` (Generic→`GenKeeloq`, FAAC→`GenFaacSLH`, KingGates→`GenKingGatesStylo4k`, Jarolift→`GenJarolift`), then `keeloq_mfr_emulate_flow = true` and `scene_manager_next_scene(SubGhzSceneSetSerial)`. Decode-only/orphan entries are listed under a trailing "Decode-only (capture to use)" section header and are non-selectable-to-generate (tapping shows a short "capture this fob to emulate" popup — no silent invalid TX).

- [ ] **Step 1: Write the scene** — copy `subghz_scene_keeloq_mfr.c` to `subghz_scene_keeloq_console.c`, rename symbols `_mfr_` → `_console_`, and change the filter from `type_encodable(type)` to the registry, adding the per-family generator selection in the submenu callback:

```c
KeeloqTypeInfo info = subghz_keeloq_registry_type_info(key->type, furi_string_get_cstr(key->name));
if(!info.encodable) { /* decode-only: show popup, return */ }
subghz->gen_info->type = info.generator; // consumed by set_serial's Gen dispatch
strncpy(subghz_keeloq_console_selected, name, sizeof(...)-1);
subghz->gen_info->keeloq.manuf = subghz_keeloq_console_selected;
subghz->keeloq_mfr_emulate_flow = true;
scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetSerial);
```
(Confirm the exact `gen_info` field that `subghz_scene_set_serial.c` reads to pick the generator — grep `gen_info->type` / `GenType` in `set_serial`/`set_counter`; use that field name.)

- [ ] **Step 2: Register the scene + hub entry** — add the `ADD_SCENE` line and the Car Keys submenu row + its handler routing to `SubGhzSceneKeeloqConsole`.

- [ ] **Step 3: Build private m5stick, verify SENTINEL_OK**

- [ ] **Step 4: Manual reachability check (documented, HW-deferred)** — note in the task report: Car Keys → KeeLoq Console lists all encodable families; selecting a FAAC/KingGates/Jarolift entry enters the correct byte-input flow.

- [ ] **Step 5: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_keeloq_console.c applications/main/subghz/scenes/subghz_scene_config.h applications/main/subghz/scenes/subghz_scene_car_keys.c
git commit  # subject: "keeloq-console Task 4: unified catalog console (browse->route)"
```

---

### Task 5: Attack path — Signal-ID naming + known-key dictionary attack

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_analyze.c`

**Interfaces:**
- Consumes: `subghz_keeloq_registry_type_info` (Task 1); the decoder's `manufacture_name` on a decoded KeeLoq; the existing Details/Emulate spokes (from the car-fob review fixes).

**Behavior:** when the analyzed capture is KeeLoq-family and decoded, show the manufacturer name + family from the registry in the Signal-ID body. Add a **Dictionary attack** action (a spoke) that, for a capture that did NOT auto-decode, iterates the keystore and reports the first manufacturer key whose decrypt yields a valid frame (serial/checksum consistent); on no match, show the honest message: `"No known key matches. Exhaustive recovery is 2^64 — not feasible on-device. Capture more or use the host correlation tool."` No progress bar.

- [ ] **Step 1: Add the registry-naming line** to the KeeLoq branch of `subghz_scene_analyze_on_enter` (use `subghz_keeloq_registry_type_info` to label the family alongside the existing `m->make`/`m->name`).

- [ ] **Step 2: Add the Dictionary-attack spoke + no-match popup** — a new `#ifndef PUBLIC_BUILD` widget button (reuse a free slot; Center is taken by the car-fob Emul spoke, so use Right if free or a submenu row) that runs the key loop and shows the result or the honest no-match text.

- [ ] **Step 3: Build private m5stick (feature target) + run the public exclusion check.** Private: `build_m5stick_s3.bat` → `SENTINEL_OK`. Public: `build_m5stick_s3_public.bat` is run ONLY to confirm this content stays OUT — verify `SENTINEL_OK` **and** `SENTINEL_VERIFY_NOCAR` → `PASS: no car material`. The public build must contain none of this (the additions are `#ifndef PUBLIC_BUILD`); a non-PASS no-car result = leak, STOP.

- [ ] **Step 4: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_analyze.c
git commit  # subject: "keeloq-console Task 5: Signal-ID naming + known-key dictionary attack"
```

---

### Task 6: Full verification, no-regression & report

**Files:**
- Create: `docs/superpowers/plans/keeloq-console-report.md`

- [ ] **Step 1: Build the two PRIVATE feature targets green + run the public exclusion check**

PowerShell, in order:
- `build_m5stick_s3.bat` (private, feature target) → `SENTINEL_OK`.
- `build_adv_only_private.bat` (private cardputer, feature target) → `SENTINEL_OK`.
- `build_m5stick_s3_public.bat` — **exclusion check only, not a ship target**: confirm `SENTINEL_OK` **and** `SENTINEL_VERIFY_NOCAR` → `PASS: no car material in ...public-merged.bin`. This proves the KeeLoq/car content stays OUT of public. A non-PASS no-car result = leak → STOP and remove the leak before continuing.

- [ ] **Step 2: Run the host tests**

`wsl.exe` run `test_keeloq_registry` (ALL PASS) and `keeloq_coverage` (0 generic FAIL; 179 generic encodable; FAAC/KingGates/Jarolift/Beninca ROUTE-OK; type-0 + KIA type-9 DECODE-ONLY).

- [ ] **Step 3: No-regression checklist** — confirm the existing "KL: By Manufacturer" picker still works (now 179), FAAC/Jarolift/KingGates SetType gen flows unchanged, VAG/KeeLoq fob emulate unchanged, `fob_sweep` summary unchanged (28/80 synth, 16/3 capture).

- [ ] **Step 4: Write the report** — coverage numbers (of 216: N encodable-generic PASS, N dedicated routed, 25 decode-only, 3 type-9 handled), what shipped, what's HW-deferred, and the honest limits (2^64 unknown-key; type-0 v1 decode-only).

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/keeloq-console-report.md
git commit  # subject: "keeloq-console Task 6: full verification + report"
```

---

## Self-review notes

- **Spec coverage:** Registry (comp.1→T1), catalog console (comp.2→T4), attack path (comp.3→T5), allow-list removal (comp.4→T3), host coverage (comp.5→T2), honest-limits (comp.6→T1 flags + T4 labels + T5 no-match). All six mapped.
- **Type consistency:** `KeeloqTypeInfo`/`KeeloqFamily`/`subghz_keeloq_registry_type_info`/`_type_encodable` used identically in T1/T2/T3/T4/T5. Generator names match `subghz_gen_info.h` verbatim.
- **Open confirmations flagged inline** (do during implementation, not blocking): the exact `gen_info` field `set_serial` reads to pick the generator (T4 Step 1); a free widget slot in the Signal-ID hub (T5 Step 2).
- **v1 scope honesty:** type-0 generate and dedicated-protocol host round-trip are explicitly deferred/out-of-harness, not silently dropped.
