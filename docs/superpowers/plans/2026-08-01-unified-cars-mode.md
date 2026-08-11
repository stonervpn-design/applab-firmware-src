# Unified "Cars" Mode + Decode Dedup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the manually-swapped `{Off, Cars 1, Cars 2}` automotive registries with a single resident `{Off, Cars}` mode, and add a lightweight decode dedup that suppresses false hits (jeep/hitag grabbing a Hyundai signal).

**Architecture:** All 61 car decoders fit resident simultaneously (~20-25 KB of ~103 KB free — measured), so the two automotive registries collapse into one; the "won't fit" split was stale. A pure-logic dedup unit, invoked at the single history-add choke point in the receiver scene, keeps only the highest-scored decode within a ~150 ms window (delete-and-replace or suppress).

**Tech Stack:** C (ESP-IDF v5.4.1, CMake), Flipper-Zero-derived SubGhz stack; Python 3 for host-side algorithm validation (this project's decoder-validation tradition — no native host C compiler is available in this environment).

## Global Constraints

- Boards: apply every change to **both** `src_m5stick_s3` and `src_cardputer_adv` (start in `src_m5stick_s3`, mirror last).
- Car decoders and their dedup score entries live under `#ifndef PUBLIC_BUILD`; public/hardened builds must still link (raw/bin_raw-only Cars).
- Build one env at a time (pioarduino/IDF SCons multi-env bug causes spurious FAILED).
- Exact protocol `.name` strings (verified): `"Jeep V0"`, `"Hitag2"` (loose/low), `"Hyundai TL"`, `"KeeLoq"`, `"Security+ 1.0"`, `"Security+ 2.0"` (specific/high).
- Dedup window: `SUBGHZ_DEDUP_WINDOW_MS = 150`.
- Do NOT commit unless the human asks; steps below stage/commit only where the human has opted in. (Repo `git` state unknown — if not a repo, skip the commit step and note it.)
- Build via the PowerShell tool, not cmd-through-bash. M5Stick = COM10, Cardputer = COM12.

---

### Task 1: Validate the dedup decision algorithm (host, Python)

Pure-logic reference for the C unit built in Task 2. Runnable now; locks the keep/replace/suppress rules and the score tiers before any C is written.

**Files:**
- Create: `docs/superpowers/plans/refs/dedup_ref.py` (reference model + asserts)

**Interfaces:**
- Produces (algorithm contract the C in Task 2 must match):
  - `score(protocol_name: str, bit_count: int) -> int`
  - `decide(state, protocol_name: str, score: int, now_ms: int) -> "add"|"replace"|"suppress"`
  - `commit(state, protocol_name, score, now_ms, history_idx)` updates `state`
  - `state = {last_protocol, last_score, last_tick, last_idx(-1)}`

- [ ] **Step 1: Write the reference model + failing tests**

```python
# docs/superpowers/plans/refs/dedup_ref.py
WINDOW_MS = 150
_LOW  = {"Jeep V0", "Hitag2"}
_HIGH = {"Hyundai TL", "KeeLoq", "Security+ 1.0", "Security+ 2.0"}

def score(name, bit_count):
    tier = 1000 if name in _LOW else 3000 if name in _HIGH else 2000
    return tier + min(bit_count, 999)   # bit-count tiebreak, never crosses a tier

def new_state():
    return {"last_protocol": None, "last_score": 0, "last_tick": 0, "last_idx": -1}

def decide(st, name, sc, now):
    if st["last_idx"] >= 0 and (now - st["last_tick"]) <= WINDOW_MS:
        return "replace" if sc > st["last_score"] else "suppress"
    return "add"

def commit(st, name, sc, now, idx):
    st.update(last_protocol=name, last_score=sc, last_tick=now, last_idx=idx)

if __name__ == "__main__":
    # 1) Hyundai then jeep inside window -> jeep suppressed
    st = new_state()
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 1000) == "add"
    commit(st, "Hyundai TL", s_h, 1000, 0)
    s_j = score("Jeep V0", 24);   assert decide(st, "Jeep V0", s_j, 1050) == "suppress"

    # 2) jeep then Hyundai inside window -> replace (specific wins even arriving 2nd)
    st = new_state()
    s_j = score("Jeep V0", 24); assert decide(st, "Jeep V0", s_j, 2000) == "add"
    commit(st, "Jeep V0", s_j, 2000, 5)
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 2040) == "replace"

    # 3) two low-tier inside window -> second suppressed (equal score, not >)
    st = new_state()
    s_j = score("Jeep V0", 24); assert decide(st, "Jeep V0", s_j, 100) == "add"
    commit(st, "Jeep V0", s_j, 100, 1)
    s_hit = score("Hitag2", 24); assert decide(st, "Hitag2", s_hit, 120) == "suppress"

    # 4) same protocol, repeat OUTSIDE window -> add (real 2nd press)
    st = new_state()
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 500) == "add"
    commit(st, "Hyundai TL", s_h, 500, 2)
    assert decide(st, "Hyundai TL", s_h, 500 + WINDOW_MS + 1) == "add"

    # 5) unrelated medium-tier protocols far apart -> both add
    st = new_state()
    s_a = score("CAME", 24); assert decide(st, "CAME", s_a, 0) == "add"
    commit(st, "CAME", s_a, 0, 3)
    s_b = score("Nice FLO", 12); assert decide(st, "Nice FLO", s_b, 5000) == "add"

    # tier ordering sanity
    assert score("Hyundai TL", 0) > score("CAME", 999) > score("Jeep V0", 999)
    print("ALL DEDUP ALGORITHM TESTS PASSED")
```

- [ ] **Step 2: Run and verify it passes**

Run: `python docs/superpowers/plans/refs/dedup_ref.py`
Expected: `ALL DEDUP ALGORITHM TESTS PASSED` (exit 0). If any assert fails, fix `decide`/`score` until green — this is the contract Task 2 ports verbatim.

- [ ] **Step 3: Commit (only if human opted into commits)**

```bash
git add docs/superpowers/plans/refs/dedup_ref.py
git commit -m "test: dedup decision algorithm reference + cases"
```

---

### Task 2: Implement the C dedup unit

Port the validated algorithm to a furi-free C unit, plus a host test mirroring the repo's `tests/*.c` convention.

**Files:**
- Create: `applications/main/subghz/helpers/subghz_decode_dedup.h`
- Create: `applications/main/subghz/helpers/subghz_decode_dedup.c`
- Create: `tests/test_subghz_decode_dedup.c`

**Interfaces:**
- Consumes: the Task 1 algorithm (tiers, window, decide rules).
- Produces (used by Task 5):
  - `typedef struct { const char* last_protocol; uint16_t last_score; uint32_t last_tick; int32_t last_history_idx; } SubGhzDecodeDedup;`
  - `typedef enum { SubGhzDedupAdd, SubGhzDedupReplace, SubGhzDedupSuppress } SubGhzDedupAction;`
  - `void subghz_decode_dedup_reset(SubGhzDecodeDedup* d);`
  - `uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count);`
  - `SubGhzDedupAction subghz_decode_dedup_decide(SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick);`
  - `void subghz_decode_dedup_commit(SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick, int32_t history_idx);`
  - `#define SUBGHZ_DEDUP_WINDOW_MS 150u`

- [ ] **Step 1: Write the header**

```c
// applications/main/subghz/helpers/subghz_decode_dedup.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SUBGHZ_DEDUP_WINDOW_MS 150u

typedef struct {
    const char* last_protocol;
    uint16_t    last_score;
    uint32_t    last_tick;
    int32_t     last_history_idx; // -1 = none committed yet
} SubGhzDecodeDedup;

typedef enum {
    SubGhzDedupAdd,       // add as a new history entry
    SubGhzDedupReplace,   // delete last_history_idx, then add the new one
    SubGhzDedupSuppress,  // skip: a better result already committed this burst
} SubGhzDedupAction;

void     subghz_decode_dedup_reset(SubGhzDecodeDedup* d);
uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count);
SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick);
void     subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t now_tick, int32_t history_idx);
```

- [ ] **Step 2: Write the host test (repo `tests/` convention: include the .c, assert, main)**

```c
// tests/test_subghz_decode_dedup.c
#include "../applications/main/subghz/helpers/subghz_decode_dedup.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    SubGhzDecodeDedup d; subghz_decode_dedup_reset(&d);
    assert(d.last_history_idx == -1);

    // tier ordering
    assert(subghz_decode_dedup_score("Hyundai TL", 0) >
           subghz_decode_dedup_score("CAME", 999));
    assert(subghz_decode_dedup_score("CAME", 999) >
           subghz_decode_dedup_score("Jeep V0", 999));

    // 1) Hyundai then jeep in-window -> suppress
    uint16_t sh = subghz_decode_dedup_score("Hyundai TL", 40);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 1000) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Hyundai TL", sh, 1000, 0);
    uint16_t sj = subghz_decode_dedup_score("Jeep V0", 24);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 1050) == SubGhzDedupSuppress);

    // 2) jeep then Hyundai in-window -> replace
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 2000) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Jeep V0", sj, 2000, 5);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 2040) == SubGhzDedupReplace);

    // 3) two low in-window -> suppress
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 100) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Jeep V0", sj, 100, 1);
    uint16_t shit = subghz_decode_dedup_score("Hitag2", 24);
    assert(subghz_decode_dedup_decide(&d, "Hitag2", shit, 120) == SubGhzDedupSuppress);

    // 4) repeat outside window -> add
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 500) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Hyundai TL", sh, 500, 2);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 500 + SUBGHZ_DEDUP_WINDOW_MS + 1)
           == SubGhzDedupAdd);

    printf("ALL C DEDUP TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Write the implementation**

```c
// applications/main/subghz/helpers/subghz_decode_dedup.c
#include "subghz_decode_dedup.h"
#include <string.h>

// Loose fixed-code decoders prone to grabbing signals that aren't theirs.
static const char* const k_low[]  = {"Jeep V0", "Hitag2"};
// Specific / rolling / checksum-validated decoders.
static const char* const k_high[] = {"Hyundai TL", "KeeLoq", "Security+ 1.0", "Security+ 2.0"};

static bool in_list(const char* n, const char* const* list, size_t count) {
    if(!n) return false;
    for(size_t i = 0; i < count; i++)
        if(strcmp(n, list[i]) == 0) return true;
    return false;
}

void subghz_decode_dedup_reset(SubGhzDecodeDedup* d) {
    d->last_protocol = NULL;
    d->last_score = 0;
    d->last_tick = 0;
    d->last_history_idx = -1;
}

uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count) {
    uint16_t tier = 2000; // medium (default)
    if(in_list(protocol_name, k_low, sizeof(k_low) / sizeof(k_low[0]))) tier = 1000;
    else if(in_list(protocol_name, k_high, sizeof(k_high) / sizeof(k_high[0]))) tier = 3000;
    uint16_t tie = bit_count < 999 ? bit_count : 999; // never crosses a tier
    return (uint16_t)(tier + tie);
}

SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick) {
    (void)protocol_name;
    if(d->last_history_idx >= 0 && (now_tick - d->last_tick) <= SUBGHZ_DEDUP_WINDOW_MS) {
        return (score > d->last_score) ? SubGhzDedupReplace : SubGhzDedupSuppress;
    }
    return SubGhzDedupAdd;
}

void subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t now_tick, int32_t history_idx) {
    d->last_protocol = protocol_name;
    d->last_score = score;
    d->last_tick = now_tick;
    d->last_history_idx = history_idx;
}
```

- [ ] **Step 4: Verify parity with the Python reference**

The C `main()` asserts mirror the five Python cases one-to-one. Re-run `python docs/superpowers/plans/refs/dedup_ref.py` and eyeball each C assert against its Python twin (same inputs → same expected action). The C host test itself compiles under the repo's normal host-test toolchain (not available in this environment); parity-by-inspection + the firmware build in Task 3 are the gates here.

- [ ] **Step 5: Commit (if opted in)**

```bash
git add applications/main/subghz/helpers/subghz_decode_dedup.h \
        applications/main/subghz/helpers/subghz_decode_dedup.c \
        tests/test_subghz_decode_dedup.c
git commit -m "feat(subghz): pure-logic decode dedup unit + host test"
```

---

### Task 3: Merge the automotive registries

Collapse groups A + B into one resident set; narrow `car_mode` to `{0,1}`.

**Files:**
- Modify: `lib/subghz/protocols/protocol_items.c:50-148`
- Verify (no change expected): `lib/subghz/protocols/protocol_items.h`

**Interfaces:**
- Consumes: existing `SubGhzProtocolRegistry`, all car-decoder `extern` symbols.
- Produces: `subghz_protocol_registry_active()` returns general for `0`, merged automotive for `1`; `subghz_protocol_registry_set_car_mode()` clamps `>=1` to `1`.

- [ ] **Step 1: Replace the two automotive arrays with one merged array**

In `protocol_items.c`, delete `subghz_protocol_registry_items_automotive_a[]` and
`...automotive_b[]` and their two `SubGhzProtocolRegistry` wrappers. Replace with a single
array containing every car decoder from BOTH former groups, exactly once, plus one `raw` and
one `bin_raw`, preserving the `#ifndef PUBLIC_BUILD` / `#else raw+bin_raw only` structure:

```c
// --- Automotive protocol set — ProtoPirate car decoders, one resident set. ---
// Measured resident footprint of all 61 decoders is ~20-25 KB (largest single is
// hitag2 ~1.8 KB; most carry no capture buffer) against ~103 KB free — the general
// set already holds ~66 decoders. The former Cars 1 / Cars 2 split was based on a
// stale "won't fit" assumption inherited from the heavyweight app decoders and is
// no longer needed.
static const SubGhzProtocol* const subghz_protocol_registry_items_automotive[] = {
#ifndef PUBLIC_BUILD
    &subghz_protocol_raw,        &subghz_protocol_bin_raw,
    // former group A
    &bmw_cas4_protocol,          &hyundai_kia_rio_protocol,
    &hyundai_tl_protocol,        &mazda_siemens_protocol,
    &sheriff_cfm_protocol,       &fiat_v1_protocol,
    &chrysler_protocol_v0,       &fiat_protocol_v0,
    &ford_protocol_v0,           &ford_protocol_v1,
    &ford_protocol_v2,           &ford_protocol_v3,
    &honda_static_protocol,      &kia_protocol_v0,
    &kia_protocol_v1,            &kia_protocol_v2,
    &kia_protocol_v3_v4,         &kia_protocol_v5,
    &kia_protocol_v6,            &kia_protocol_v7,
    &land_rover_v0_protocol,     &mazda_v0_protocol,
    &mitsubishi_v0_protocol,     &porsche_touareg_protocol,
    &psa_protocol,               &subghz_protocol_scher_khan,
    &subghz_protocol_star_line,  &subaru_protocol,
    &vag_protocol,               &subghz_protocol_audi_v1,
    &subghz_protocol_audi_v2,
    // former group B
    &subghz_protocol_audi_v3,        &subghz_protocol_bmw_v2,
    &subghz_protocol_bmw_v3,         &subghz_protocol_chevrolet_v0,
    &subghz_protocol_ducato_v0,      &subghz_protocol_fiat_spa,
    &subghz_protocol_fiat_v2_v3_v5,  &subghz_protocol_hitag2,
    &subghz_protocol_honda_v2_v3,    &subghz_protocol_honda_v4,
    &subghz_protocol_honda_v5,       &subghz_protocol_honda_v6,
    &subghz_protocol_honda_v7,       &subghz_protocol_honda_v8,
    &subghz_protocol_honda_v9,       &subghz_protocol_honda_v10,
    &subghz_protocol_jeep_v0,        &subghz_protocol_ldv_v0,
    &subghz_protocol_lexus_v0,       &subghz_protocol_mazda_v1,
    &subghz_protocol_mazda_v2,       &subghz_protocol_mitsubishi_v1,
    &subghz_protocol_nissan_v0,      &subghz_protocol_opel_v0,
    &subghz_protocol_peugot_v0,      &subghz_protocol_renault_v1,
    &subghz_protocol_renault_v2,     &subghz_protocol_renault_v3,
    &subghz_protocol_renault_v4,     &subghz_protocol_toyota_v0,
#else
    &subghz_protocol_raw,
    &subghz_protocol_bin_raw,
#endif
};

static const SubGhzProtocolRegistry subghz_protocol_registry_automotive = {
    .items = subghz_protocol_registry_items_automotive,
    .size = COUNT_OF(subghz_protocol_registry_items_automotive)};
```

- [ ] **Step 2: Narrow the mode selector to 0/1**

Replace the selector block (`s_subghz_car_mode` + the four functions) with:

```c
// Car mode: 0 = general (Off), 1 = Cars (single merged automotive set).
static uint8_t s_subghz_car_mode = 0;

uint8_t subghz_protocol_registry_car_mode(void) {
    return s_subghz_car_mode;
}

void subghz_protocol_registry_set_car_mode(uint8_t mode) {
    // Any nonzero (incl. a stale saved "Cars 2" == 2) maps to the single Cars mode.
    s_subghz_car_mode = (mode >= 1) ? 1 : 0;
}

bool subghz_protocol_registry_automotive_mode(void) {
    return s_subghz_car_mode != 0;
}

const SubGhzProtocolRegistry* subghz_protocol_registry_active(void) {
    return (s_subghz_car_mode == 1) ? &subghz_protocol_registry_automotive
                                     : &subghz_protocol_registry;
}
```

- [ ] **Step 3: Build the m5stick private firmware to verify it compiles/links**

Run (PowerShell tool, from `src_m5stick_s3`): the existing private build script (`build.sh`
private path or `build_adv_only_private.bat` equivalent for this tree — use whichever the
tree already uses for the m5stick private merged bin).
Expected: clean build, no undefined-symbol/link errors, `SENTINEL_OK`-equivalent success.
If a former-group-B symbol fails to link, it was only reachable via the old array — add it to
the merged array above.

- [ ] **Step 4: Commit (if opted in)**

```bash
git add lib/subghz/protocols/protocol_items.c
git commit -m "feat(subghz): merge Cars 1/2 into one resident automotive registry"
```

---

### Task 4: Update the UI selector to {Off, Cars}

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_radio_settings.c:167-177`

**Interfaces:**
- Consumes: `subghz_txrx_set_car_mode`, `subghz_txrx_get_car_mode` (unchanged).

- [ ] **Step 1: Replace the mode text array and stale comment**

```c
static const char* const car_mode_text[] = {"Off", "Cars"};
#define CAR_MODE_COUNT (sizeof(car_mode_text) / sizeof(car_mode_text[0]))

static void subghz_scene_radio_settings_set_automotive(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, car_mode_text[index]);
    // Single merged automotive set (all car decoders resident); rebuilds the receiver.
    subghz_txrx_set_car_mode(subghz->txrx, index);
}
```

Also: at the read-back (~line 194-196) a stale saved index of `2` must not index out of
`car_mode_text[]`. Clamp on read:

```c
    value_index = subghz_txrx_get_car_mode(subghz->txrx);
    if(value_index >= CAR_MODE_COUNT) value_index = 1; // stale "Cars 2" -> Cars
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, car_mode_text[value_index]);
```

- [ ] **Step 2: Update the stale comment on `subghz_txrx_set_car_mode`**

In `applications/main/subghz/helpers/subghz_txrx.c` (the `set_car_mode` body, ~line 199-211),
replace the "split in two / Cars 1 / Cars 2 / no-PSRAM can't hold them together" comment with:

```c
    // Rebuild the receiver from the newly selected registry (general vs the single
    // merged automotive set). Caller must have RX stopped.
```

The logic inside is unchanged (it already frees + rebuilds from `registry_active()`).

- [ ] **Step 3: Build m5stick private; verify compile + that Radio Settings shows Off/Cars**

Run the private build. Expected: clean build. (On-device menu check happens in Task 7.)

- [ ] **Step 4: Commit (if opted in)**

```bash
git add applications/main/subghz/scenes/subghz_scene_radio_settings.c \
        applications/main/subghz/helpers/subghz_txrx.c
git commit -m "feat(subghz): Automotive selector becomes {Off, Cars}"
```

---

### Task 5: Wire dedup into the receiver history callback

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_receiver.c:100-157` (callback) and the
  scene `on_enter` (reset dedup)
- Modify: `applications/main/subghz/subghz_i.h` (add `SubGhzDecodeDedup dedup;` to the `SubGhz`
  struct — confirm exact struct/file during implementation; it is the app-wide context that
  already owns `history`, `txrx`, `notifications`).

**Interfaces:**
- Consumes: Task 2's `subghz_decode_dedup_*`; existing `subghz_history_add_to_history`,
  `subghz_history_delete_item`, `subghz_view_receiver_delete_item`, `subghz_history_get_item`.

- [ ] **Step 1: Add the dedup field + include**

In `subghz_i.h`, add `#include "helpers/subghz_decode_dedup.h"` and a member
`SubGhzDecodeDedup dedup;` to the `SubGhz` struct. In `subghz_scene_receiver_on_enter`
(near where `subghz_txrx_set_rx_callback(... add_to_history_callback ...)` is set, ~line 204),
add `subghz_decode_dedup_reset(&subghz->dedup);`.

- [ ] **Step 2: Insert the dedup decision in the callback (before the add)**

In `subghz_scene_add_to_history_callback`, inside the `if((... & ignore_filter) == 0)` block,
replace the add region (currently `uint16_t idx = subghz_history_get_item(history);` … the
`if(subghz_history_add_to_history(...))` block) with dedup-aware flow:

```c
        SubGhzHistory* history = subghz->history;
        FuriString* item_name = furi_string_alloc();
        FuriString* item_time = furi_string_alloc();

        // --- dedup: score this decode and decide keep/replace/suppress ---
        const char* proto_name = decoder_base->protocol->name;
        uint16_t bit_count = subghz_history_get_last_bit_count(history, decoder_base);
        uint16_t score = subghz_decode_dedup_score(proto_name, bit_count);
        uint32_t now = furi_get_tick();
        SubGhzDedupAction act =
            subghz_decode_dedup_decide(&subghz->dedup, proto_name, score, now);

        if(act == SubGhzDedupSuppress) {
            subghz_receiver_reset(receiver);
            furi_string_free(item_name);
            furi_string_free(item_time);
            return;
        }
        if(act == SubGhzDedupReplace && subghz->dedup.last_history_idx >= 0) {
            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);
            subghz_history_delete_item(history, subghz->dedup.last_history_idx);
            subghz_view_receiver_delete_item(
                subghz->subghz_receiver, subghz->dedup.last_history_idx);
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);
        }

        uint16_t idx = subghz_history_get_item(history);
        SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
        if(subghz->last_settings->delete_old_signals) {
            if(subghz_history_get_last_index(subghz->history) >= 54) {
                subghz->state_notifications = SubGhzNotificationStateRx;
                subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);
                subghz_history_delete_item(subghz->history, 0);
                subghz_view_receiver_delete_item(subghz->subghz_receiver, 0);
                subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);
                subghz_scene_receiver_update_statusbar(subghz);
                subghz->idx_menu_chosen =
                    subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
                idx--;
            }
        }
        if(subghz_history_add_to_history(history, decoder_base, &preset)) {
            furi_string_reset(item_name);
            furi_string_reset(item_time);
            subghz->state_notifications = SubGhzNotificationStateRxDone;
            subghz_history_get_text_item_menu(history, item_name, idx);
            subghz_history_get_time_item_menu(history, item_time, idx);
            subghz_view_receiver_add_item_to_menu(
                subghz->subghz_receiver,
                furi_string_get_cstr(item_name),
                furi_string_get_cstr(item_time),
                subghz_history_get_type_protocol(history, idx));
            subghz_scene_receiver_update_statusbar(subghz);
            if(subghz_history_get_text_space_left(subghz->history, NULL)) {
                notification_message(subghz->notifications, &sequence_error);
            }
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
            // record what we committed so the next in-window decode can compare
            subghz_decode_dedup_commit(&subghz->dedup, proto_name, score, now, idx);
        }
        subghz_receiver_reset(receiver);
        furi_string_free(item_name);
        furi_string_free(item_time);
```

- [ ] **Step 3: Resolve the bit-count accessor**

`subghz_history_get_last_bit_count(...)` above is illustrative. During implementation, get the
decoded bit count from whatever accessor the codebase already exposes on `decoder_base` /
generic (e.g. a `data_count_bit` getter). If none exists cheaply, pass `bit_count = 0` — the
score's tier term still does all the real disambiguation; the bit-count is only a same-tier
tiebreak. Do NOT invent a new low-level API for this.

- [ ] **Step 4: Build m5stick private; verify compile**

Run the private build. Expected: clean build/link.

- [ ] **Step 5: Commit (if opted in)**

```bash
git add applications/main/subghz/scenes/subghz_scene_receiver.c \
        applications/main/subghz/subghz_i.h
git commit -m "feat(subghz): dedup near-simultaneous decodes in receiver history"
```

---

### Task 6: Mirror all changes into src_cardputer_adv

**Files:**
- Create: `../src_cardputer_adv/applications/main/subghz/helpers/subghz_decode_dedup.{h,c}`
- Create: `../src_cardputer_adv/tests/test_subghz_decode_dedup.c`
- Modify: `../src_cardputer_adv/lib/subghz/protocols/protocol_items.c`
- Modify: `../src_cardputer_adv/applications/main/subghz/scenes/subghz_scene_radio_settings.c`
- Modify: `../src_cardputer_adv/applications/main/subghz/helpers/subghz_txrx.c`
- Modify: `../src_cardputer_adv/applications/main/subghz/scenes/subghz_scene_receiver.c`
- Modify: `../src_cardputer_adv/applications/main/subghz/subghz_i.h`

**Interfaces:** identical to Tasks 2-5.

- [ ] **Step 1: Copy the two dedup files + host test verbatim**

The dedup unit is board-agnostic — copy `subghz_decode_dedup.{h,c}` and
`test_subghz_decode_dedup.c` unchanged into the cardputer tree.

- [ ] **Step 2: Apply Tasks 3-5 edits to the cardputer copies**

Diff the cardputer tree's `protocol_items.c`, `subghz_scene_radio_settings.c`, `subghz_txrx.c`,
`subghz_scene_receiver.c`, `subghz_i.h` against the m5stick versions first; the automotive
arrays and callback should already match (car features are kept in sync across both private
trees). Apply the same three edits (merged registry, `{Off, Cars}` selector + clamp, dedup
wiring). If the cardputer tree's former group arrays differ from m5stick's, merge whatever
decoders it actually had — do not blindly paste m5stick's list.

- [ ] **Step 3: Build the cardputer private firmware**

Run (PowerShell tool, from `src_cardputer_adv`): `build_adv_only_private.bat` (one env only).
Expected: clean build/link.

- [ ] **Step 4: Commit (if opted in)**

```bash
git add ../src_cardputer_adv/applications/main/subghz \
        ../src_cardputer_adv/lib/subghz/protocols/protocol_items.c \
        ../src_cardputer_adv/tests/test_subghz_decode_dedup.c
git commit -m "feat(subghz): mirror unified Cars mode + dedup into cardputer_adv"
```

---

### Task 7: On-device acceptance + heap check (both boards)

**Files:** none (verification only).

- [ ] **Step 1: Flash both boards**

M5Stick: `flash_m5stick_com10.bat` (COM10). Cardputer: `flash_cardputer_com12.bat` (COM12).
Confirm `SENTINEL_OK` in each `.log`.

- [ ] **Step 2: No-swap decode**

On each board: `Radio Settings → Automotive → Cars`. From a second unit, TX a Hyundai TL
(former group A) and a Jeep/Honda (former group B) back-to-back **without changing the mode**.
Expected: both appear in the receiver history.

- [ ] **Step 3: Dedup**

TX a Hyundai TL and watch the history. Expected: a single `Hyundai TL` entry, not
`Jeep V0 ×3` + `Hitag2 ×1`.

- [ ] **Step 4: Heap stability**

Over serial, watch free heap across ~20 RX events and several `Off`↔`Cars` toggles.
Expected: heap returns to a stable baseline each cycle (no per-rebuild leak), comfortably above
zero (target: tens of KB free during Cars RX).

- [ ] **Step 5: Regression**

`Off` (general) mode decodes a native protocol (e.g. Princeton/CAME) unchanged. Confirm a
public/hardened build (if produced) still links and shows `{Off, Cars}` with raw-only Cars.

- [ ] **Step 6: Record result**

Note pass/fail per step. If dedup over-suppresses a legitimately distinct second signal,
lower `SUBGHZ_DEDUP_WINDOW_MS` (e.g. 100) and re-flash; if collisions slip through, raise it
(e.g. 200) — the window is the single tuning knob.

---

## Self-Review

**Spec coverage:**
- Merge registries → Task 3 ✓
- car_mode 0/1 + stale-2 clamp → Task 3 (setter) + Task 4 (UI read clamp) ✓
- UI {Off, Cars} → Task 4 ✓
- Dedup unit (score table, window, replace/suppress) → Tasks 1-2 ✓
- Dedup integration at history choke point → Task 5 ✓
- Both trees / PUBLIC_BUILD guards → Tasks 3-6 ✓
- Testing (no-swap, dedup, heap, regression) → Task 7 ✓
- Rollback (single-commit revert; dedup additive) → covered by per-task commits ✓

**Placeholder scan:** No TBD/TODO. The one deferred detail (bit-count accessor, Task 5 Step 3)
is explicitly handled with a safe fallback (`bit_count = 0`), not left open. Build-script exact
name is given as the tree's existing private build script because both trees ship more than one
and the correct one is the private-merged variant.

**Type consistency:** `SubGhzDecodeDedup`, `SubGhzDedupAction` enum values, and all five
`subghz_decode_dedup_*` signatures match between Task 2 (definition), the Task 2 host test, and
Task 5 (call sites). `SUBGHZ_DEDUP_WINDOW_MS` used consistently. Protocol name strings match the
verified `.name` values.
