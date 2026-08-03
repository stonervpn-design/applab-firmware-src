# KeeLoq No-Desync Clone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an algorithmic no-desync KeeLoq clone — a stored fob identity (derived device key `kdev` + serial + last counter) that transmits synthesized future counter frames (a one-time forward jump, then a back-to-back `[N, N+1]` resync pair) so the target receiver always accepts and never desyncs.

**Architecture:** Reuse the existing by-manufacturer KeeLoq engine unchanged for per-manufacturer plaintext assembly and cipher. Add (1) a pure, host-tested counter core that computes the forward pair; (2) a `clone_active` branch in the encoder that pins the counter (no drift), emits the pair in one upload, and can use a stored `kdev` when the keystore lacks the key; (3) new `.sub` fields persisting the clone; (4) two UI entry points (from a capture, and from By-Manufacturer). Private trees only.

**Tech Stack:** C (Flipper firmware port on ESP32-S3 / pioarduino), FlipperFormat `.sub` files, host unit tests via WSL gcc.

## Global Constraints

- **Boards:** apply every change to BOTH `src_m5stick_s3` and `src_cardputer_adv`. Build ONE env at a time (SCons bug). Public/hardened trees: all new code guarded by `#ifndef PUBLIC_BUILD`.
- **Cipher reality:** `subghz_protocol_keeloq_common_encrypt`/`_decrypt` (keeloq_common.c:16/30) are real 528-round NLFSR functions and DO run. Only the AES *keystore-file* decryption is stubbed on this port; it is irrelevant because clones use plaintext `Encryption:0` manufacturer keys. Do NOT "fix" or route around the cipher.
- **Per-mfr plaintext assembly is load-bearing:** the `decrypt` word (keeloq.c:350-428) uses manufacturer-specific serial masks / fixed discriminators keyed on `instance->manufacture_name`. The clone MUST keep `manufacture_name` set and MUST NOT bypass this block. `kdev` replaces only the key-*derivation* switch (keeloq.c:431-527).
- **Counter is 16-bit**, wraps at `0xFFFF` (modular). Default clone jump = `100`.
- **Host test command** (Git Bash has no gcc — use WSL), run from the tree root:
  `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra -o /tmp/t tests/<test>.c && /tmp/t"`
- **Compile gate:** `build_ci.bat` at each tree root. Firmware auto-globs `applications/main/subghz/**` and `lib/subghz/protocols/**` (no `sources=` in `application.fam`), so new `.c` files there compile with no manifest edit.
- **Commit** after each task with the message shown. End commit messages with the two trailers used in this repo (`Co-Authored-By:` / `Claude-Session:`).

**Learning-type IDs** (keeloq_common.h:19-34, referenced throughout):
`SIMPLE=1 NORMAL=2 SECURE=3 MAGIC_XOR_TYPE_1=4 FAAC=5 MAGIC_SERIAL_1/2/3=6/7/8 KINGGATES=10 JAROLIFT=11 ERREKA=12 PUJOL=13 AERF=14 SIMPLE_JCM=15`.

---

## File Structure

**Create (both trees):**
- `lib/subghz/protocols/subghz_keeloq_clone_core.h` — pure counter-core types + prototypes (hardware-free: only `<stdint.h>/<stdbool.h>`).
- `lib/subghz/protocols/subghz_keeloq_clone_core.c` — pure counter algorithm (no furi).
- `tests/test_keeloq_clone_core.c` — host unit test (`#include`s the core `.c`).
- `applications/main/subghz/scenes/subghz_scene_set_jump.c` — ByteInput scene for the jump value.

**Modify (both trees):**
- `lib/subghz/protocols/keeloq.c` — encoder struct fields; kdev capture + fallback in `gen_data`; forward-pair branch in `get_upload`; serialize/deserialize clone fields; Cnt write-back.
- `applications/main/subghz/helpers/subghz_txrx_create_protocol_key.{c,h}` — `subghz_txrx_gen_keeloq_clone(...)`.
- `applications/main/subghz/helpers/subghz_txrx.{c,h}` — expose the encoder instance if needed to read back `kdev` (see Task 6; a getter may be added).
- `applications/main/subghz/subghz_i.h` — `keeloq_clone_flow` + `keeloq_clone_jump` fields; a `keeloq.jump` gen_info field.
- `applications/main/subghz/helpers/subghz_gen_info.h` — add `uint16_t jump;` to the keeloq gen_info arm.
- `applications/main/subghz/scenes/subghz_scene_keeloq_action.c` — third "Clone (no-desync)" item.
- `applications/main/subghz/scenes/subghz_scene_set_counter.c` — route KeeLoq clone flow to SetJump.
- `applications/main/subghz/scenes/subghz_scene_receiver_info.c` — "Clone" action for dynamic KeeLoq captures.
- `applications/main/subghz/scenes/subghz_scene_config.h` — register `set_jump`.

---

## Task 1: Pure clone counter core (host-tested)

The only genuinely new logic: given clone state, produce the forward `[a,b]` pair. Pure integer math — fully host-testable.

**Files:**
- Create: `lib/subghz/protocols/subghz_keeloq_clone_core.h`
- Create: `lib/subghz/protocols/subghz_keeloq_clone_core.c`
- Test: `tests/test_keeloq_clone_core.c`

**Interfaces:**
- Produces: `typedef struct { uint32_t serial; uint8_t btn; uint64_t kdev; uint8_t learn_type; uint16_t last_cnt; uint16_t jump; bool synced; } KeeloqClone;`
- Produces: `void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_keeloq_clone_core.c`:
```c
#include "../lib/subghz/protocols/subghz_keeloq_clone_core.c"
#include <assert.h>
#include <stdio.h>

static KeeloqClone mk(uint16_t last, uint16_t jump, bool synced) {
    KeeloqClone c = {0};
    c.serial = 0x0ABCDEF; c.btn = 2; c.kdev = 0x1122334455667788ull;
    c.learn_type = 2; c.last_cnt = last; c.jump = jump; c.synced = synced;
    return c;
}

int main(void) {
    uint16_t a = 0, b = 0;

    // Fresh clone: one-time jump lead, then synced.
    KeeloqClone c1 = mk(5, 100, false);
    keeloq_clone_next_pair(&c1, 0, &a, &b);
    assert(a == 105 && b == 106 && c1.last_cnt == 106 && c1.synced == true);

    // Subsequent send: +2 pair advance (no re-jump).
    keeloq_clone_next_pair(&c1, 0, &a, &b);
    assert(a == 107 && b == 108 && c1.last_cnt == 108);

    // Fresh capture ahead of stored last -> catch up before jump.
    KeeloqClone c2 = mk(5, 100, false);
    keeloq_clone_next_pair(&c2, 500, &a, &b);
    assert(a == 600 && b == 601 && c2.last_cnt == 601 && c2.synced == true);

    // 16-bit wrap on the +1 pair.
    KeeloqClone c3 = mk(0xFFFF, 100, true);
    keeloq_clone_next_pair(&c3, 0, &a, &b);
    assert(a == 0x0000 && b == 0x0001 && c3.last_cnt == 0x0001);

    // Jump that overflows 16 bits wraps modularly.
    KeeloqClone c4 = mk(0xFFF0, 0x100, false);
    keeloq_clone_next_pair(&c4, 0, &a, &b);
    assert(a == 0x00F0 && b == 0x00F1);

    printf("test_keeloq_clone_core OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra -o /tmp/t tests/test_keeloq_clone_core.c && /tmp/t"`
Expected: FAIL — `subghz_keeloq_clone_core.c: No such file` (or undefined `keeloq_clone_next_pair`).

- [ ] **Step 3: Write the header**

Create `lib/subghz/protocols/subghz_keeloq_clone_core.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pure, hardware-free no-desync clone state + counter algorithm.
// No furi / furi_hal / FreeRTOS references (host-testable).
typedef struct {
    uint32_t serial;    // 28-bit fob serial
    uint8_t  btn;       // 4-bit button nibble
    uint64_t kdev;      // derived device key (final `man` fed to KeeLoq encrypt); 0 = absent
    uint8_t  learn_type;// KEELOQ_LEARNING_* id (for display / seed handling)
    uint16_t last_cnt;  // persisted last counter emitted (the +1 of the last pair)
    uint16_t jump;      // one-time forward lead applied until synced
    bool     synced;    // has the one-time jump been applied yet
} KeeloqClone;

// Compute the next no-desync frame pair.
// - If !synced: target = max(last_cnt, seen_cnt) + jump, then synced := true (one-time lead).
// - Else:       target = last_cnt + 1 (pair advances the counter by +2 per send).
// Writes target -> *out_a and (target+1) -> *out_b (both 16-bit modular),
// and updates c->last_cnt = *out_b. seen_cnt is a freshly-observed live-fob counter
// (pass 0 or last_cnt when re-opening without a re-capture).
void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b);
```

- [ ] **Step 4: Write the implementation**

Create `lib/subghz/protocols/subghz_keeloq_clone_core.c`:
```c
#include "subghz_keeloq_clone_core.h"

void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b) {
    uint16_t base = c->last_cnt;
    if(seen_cnt > base) base = seen_cnt; // catch up to a fresh live capture that is ahead

    uint16_t target;
    if(!c->synced) {
        target = (uint16_t)(base + c->jump); // one-time forward lead (modular)
        c->synced = true;
    } else {
        target = (uint16_t)(base + 1); // pair advances counter by +2 per send
    }

    *out_a = target;
    *out_b = (uint16_t)(target + 1);
    c->last_cnt = *out_b;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra -o /tmp/t tests/test_keeloq_clone_core.c && /tmp/t"`
Expected: PASS — prints `test_keeloq_clone_core OK`.

- [ ] **Step 6: Commit**

```bash
git add lib/subghz/protocols/subghz_keeloq_clone_core.h lib/subghz/protocols/subghz_keeloq_clone_core.c tests/test_keeloq_clone_core.c
git commit -m "feat(keeloq): pure no-desync clone counter core + host test"
```

---

## Task 2: Encoder struct fields + kdev capture/fallback in gen_data

Add clone state to the encoder, capture the derived key at build time, and allow a stored `kdev` to drive encryption when the keystore lacks the manufacturer key. The per-mfr `decrypt` assembly (keeloq.c:350-428) stays untouched.

**Files:**
- Modify: `lib/subghz/protocols/keeloq.c` (struct ~41-51; gen_data derivation region 431-536)

**Interfaces:**
- Consumes: `KeeloqClone` (Task 1).
- Produces: encoder fields `clone_active`, `clone` (a `KeeloqClone`), `last_derived_man`; behavior: when `clone_active && clone.kdev != 0` and no keystore match set `hop`, encrypt with `clone.kdev`; always record the derivation result into `last_derived_man`.

- [ ] **Step 1: Add the core include and struct fields**

At the top includes of `keeloq.c` add:
```c
#include "subghz_keeloq_clone_core.h"
```
Change the encoder struct (currently keeloq.c:41-51):
```c
struct SubGhzProtocolEncoderKeeloq {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    SubGhzKeystore* keystore;
    const char* manufacture_name;

    FuriString* manufacture_from_file;

    // No-desync clone state (feature: no-desync clone).
    bool clone_active;         // TRUE when transmitting as a no-desync clone
    KeeloqClone clone;         // serial/btn/kdev/learn_type/last_cnt/jump/synced
    uint64_t last_derived_man; // captures the `man` used by the last gen_data (for clone-build)
};
```
In `subghz_protocol_encoder_keeloq_alloc` (search for the encoder alloc that sets `instance->manufacture_from_file = furi_string_alloc();`), add after that line so the build path (create_data, which does NOT go through deserialize) starts clean:
```c
    instance->clone_active = false;
    instance->clone = (KeeloqClone){0};
    instance->last_derived_man = 0;
```

- [ ] **Step 2: Capture the derived key and add the kdev fallback**

In `subghz_protocol_keeloq_gen_data`, the derivation loop currently ends at keeloq.c:528 (`}` closing the `else` that wraps the `for M_EACH` scan), immediately before:
```c
    if(hop || (prog_mode == PROG_MODE_KEELOQ_DEA_MIO) || (prog_mode == PROG_MODE_KEELOQ_BFT)) {
```
Insert, directly before that `if(hop || ...)` line:
```c
    // No-desync clone: keystore-independent fallback. The per-mfr `decrypt`
    // plaintext (assembled above from manufacture_name) is already correct;
    // if the keystore had no matching key (hop still 0) but this instance holds
    // a stored device key, encrypt with it directly. Also record the derived
    // key so clone-build can persist it.
    if(instance->clone_active && instance->clone.kdev != 0) {
        if(hop == 0) {
            hop = subghz_protocol_keeloq_common_encrypt(decrypt, instance->clone.kdev);
        }
        instance->last_derived_man = instance->clone.kdev;
    } else if(man != 0) {
        instance->last_derived_man = man; // SIMPLE etc. leave man==0 (key used directly)
    }
```
Note: `decrypt`, `hop`, `man` are all in scope here (declared at keeloq.c:205-206 and 350). For SIMPLE/SIMPLE_JCM/UNKNOWN-type1 the key is applied directly as the encrypt key (no `man`); clone-build (Task 6) captures `kdev` for those by reading it from the keystore entry, not from `last_derived_man` — see Task 6.

- [ ] **Step 3: Compile-gate**

Run: `build_ci.bat` (from tree root).
Expected: links clean; no new warnings from the added fields/branch.

- [ ] **Step 4: Commit**

```bash
git add lib/subghz/protocols/keeloq.c
git commit -m "feat(keeloq): encoder clone state + kdev capture/fallback in gen_data"
```

---

## Task 3: Forward-pair emit in get_upload

When `clone_active`, build exactly two frames `[a, b]` (from the counter core) into one upload buffer with `counter_up=false` (no drift), instead of the normal single-frame / mode-7 paths.

**Files:**
- Modify: `lib/subghz/protocols/keeloq.c` (`subghz_protocol_encoder_keeloq_get_upload`, 665-710)

**Interfaces:**
- Consumes: `keeloq_clone_next_pair` (Task 1); encoder `clone` state (Task 2); `subghz_protocol_encoder_keeloq_encode_to_timings` (existing, keeloq.c:603).
- Produces: a 2-frame upload; `instance->clone.last_cnt` advanced; `instance->generic.cnt` left at `b`.

- [ ] **Step 1: Add the clone branch**

In `subghz_protocol_encoder_keeloq_get_upload`, immediately after the `bypass` set block (keeloq.c:673-675) and BEFORE the `if(keeloq_counter_mode == 7 && !bypass)` line, insert:
```c
    // No-desync clone: emit the forward pair [a, b] back-to-back in one upload.
    // counter_up=false so gen_data uses generic.cnt verbatim (no drift).
    if(instance->clone_active) {
        uint16_t a = 0, b = 0;
        keeloq_clone_next_pair(&instance->clone, (uint16_t)instance->generic.cnt, &a, &b);

        instance->encoder.repeat = 2;
        instance->generic.cnt = a;
        upindex = subghz_protocol_encoder_keeloq_encode_to_timings(
            instance, instance->clone.btn, false, upindex);
        instance->generic.cnt = b;
        upindex = subghz_protocol_encoder_keeloq_encode_to_timings(
            instance, instance->clone.btn, false, upindex);
        instance->encoder.size_upload = upindex;
        return true;
    }
```
`upindex` is declared at keeloq.c:670 (`size_t upindex = 0;`), in scope.

- [ ] **Step 2: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link.

- [ ] **Step 3: Commit**

```bash
git add lib/subghz/protocols/keeloq.c
git commit -m "feat(keeloq): forward-pair (N,N+1) upload for no-desync clone"
```

---

## Task 4: Serialize clone fields

Persist the clone as self-contained `.sub` fields. Uses `insert_or_update_*` (idempotent) so re-saves don't duplicate keys.

**Files:**
- Modify: `lib/subghz/protocols/keeloq.c` (`subghz_protocol_decoder_keeloq_serialize`, 1420-1452)

**Interfaces:**
- Produces `.sub` fields when the encoder is a clone: `CloneMode(1) Serial Btn Cnt Kdev(hex8) LearnType CloneJump CloneSynced`. `Manufacture` continues to be written by the existing code.
- Note: serialize runs on the DECODER instance; a clone is transmitted from the ENCODER. Persist from the clone-build helper (Task 6) which writes directly into `fff_data`. This task adds the READ side helper the loader needs — see Task 5 — plus documents the field set. No decoder-serialize change is required for the clone path (the clone `.sub` is authored by Task 6). **Skip decoder-serialize edits; this task is folded into Task 6.**

- [ ] **Step 1: (folded)** — No standalone change. Proceed to Task 5. This task exists to record that clone `.sub` authoring lives in the txrx helper (Task 6), not decoder-serialize, because clones are generated (encoder-side), never saved from a decode via `serialize`.

---

## Task 5: Deserialize clone fields + Cnt write-back

Teach the ENCODER loader to recognize a clone `.sub`, populate `clone_active` + `clone` state, keep `manufacture_name` (for the per-mfr assembly), and rewrite `Cnt` after each send so re-opening resumes forward.

**Files:**
- Modify: `lib/subghz/protocols/keeloq.c` (`subghz_protocol_encoder_keeloq_deserialize`, 712-811)

**Interfaces:**
- Consumes `.sub` fields authored by Task 6.
- Produces: on load of a clone, `instance->clone_active=true` and `instance->clone` fully populated; after `get_upload`, `Cnt`/`CloneSynced` updated in `flipper_format`.

- [ ] **Step 1: Read clone fields before check_remote_controller**

In `subghz_protocol_encoder_keeloq_deserialize`, the `CounterMode` read block is at keeloq.c:774-779:
```c
        uint32_t tmp_counter_mode;
        if(flipper_format_read_uint32(flipper_format, "CounterMode", &tmp_counter_mode, 1)) {
            keeloq_counter_mode = (uint8_t)tmp_counter_mode;
        } else {
            keeloq_counter_mode = 0;
        }
```
Immediately AFTER that block, insert (each keyed read is preceded by a rewind, matching the file's convention):
```c
        // No-desync clone fields (feature: no-desync clone).
        instance->clone_active = false;
        instance->clone = (KeeloqClone){0};
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        uint32_t tmp_clone_mode = 0;
        if(flipper_format_read_uint32(flipper_format, "CloneMode", &tmp_clone_mode, 1) &&
           tmp_clone_mode == 1) {
            uint32_t v = 0;
            uint8_t kdev_data[sizeof(uint64_t)] = {0};

            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "Serial", &v, 1))
                instance->clone.serial = v;
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "Btn", &v, 1))
                instance->clone.btn = (uint8_t)v;
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "Cnt", &v, 1))
                instance->clone.last_cnt = (uint16_t)v;
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "LearnType", &v, 1))
                instance->clone.learn_type = (uint8_t)v;
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "CloneJump", &v, 1))
                instance->clone.jump = (uint16_t)v;
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_uint32(flipper_format, "CloneSynced", &v, 1))
                instance->clone.synced = (v != 0);
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_hex(flipper_format, "Kdev", kdev_data, sizeof(uint64_t))) {
                uint64_t k = 0;
                for(size_t i = 0; i < sizeof(uint64_t); i++) k = (k << 8) | kdev_data[i];
                instance->clone.kdev = k;
            }
            instance->clone.btn = (uint8_t)(instance->clone.btn & 0x0F);
            instance->generic.serial = instance->clone.serial;
            instance->generic.btn = instance->clone.btn;
            instance->generic.cnt = instance->clone.last_cnt;
            instance->clone_active = true;
        }
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
```

- [ ] **Step 2: Write Cnt/CloneSynced back after get_upload**

Still in `subghz_protocol_encoder_keeloq_deserialize`, the existing `Key` update block is at keeloq.c:800-807:
```c
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Unable to add Key");
            ret = SubGhzProtocolStatusErrorParserKey;
            break;
        }
```
Immediately AFTER that block, insert:
```c
        if(instance->clone_active) {
            uint32_t cnt_v = instance->clone.last_cnt;
            uint32_t synced_v = instance->clone.synced ? 1 : 0;
            flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_v, 1);
            flipper_format_insert_or_update_uint32(flipper_format, "CloneSynced", &synced_v, 1);
        }
```

- [ ] **Step 3: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link.

- [ ] **Step 4: Commit**

```bash
git add lib/subghz/protocols/keeloq.c
git commit -m "feat(keeloq): load no-desync clone fields + persist advanced counter"
```

---

## Task 6: txrx clone generator helper

Author a clone from `(serial, btn, cnt, jump, manufacture_name)`: run the existing keystore path once to produce a valid frame and capture `kdev`, then write the clone fields into `fff_data`.

**Files:**
- Modify: `lib/subghz/protocols/keeloq.c` — add `subghz_protocol_keeloq_clone_capture_kdev(...)` helper + declaration in `keeloq.h`.
- Modify: `applications/main/subghz/helpers/subghz_txrx_create_protocol_key.{c,h}` — add `subghz_txrx_gen_keeloq_clone(...)`.

**Interfaces:**
- Consumes: `subghz_protocol_keeloq_create_data` (keeloq.c:539); `subghz_protocol_encoder_keeloq` instance; `last_derived_man` (Task 2).
- Produces: `bool subghz_txrx_gen_keeloq_clone(SubGhzTxRx* instance, const char* preset_name, uint32_t frequency, uint32_t serial, uint8_t btn, uint16_t cnt, uint16_t jump, const char* manufacture_name);`

- [ ] **Step 1: Add a kdev-capture accessor in keeloq.c**

Add to `keeloq.c` (near `create_data`) and declare in `keeloq.h`:
```c
// Returns the device key (`man`) that gen_data derived on its most recent run for
// this encoder instance, or 0 if the manufacturer uses the key directly (SIMPLE etc.).
uint64_t subghz_protocol_keeloq_encoder_last_kdev(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKeeloq* instance = context;
    return instance->last_derived_man;
}

// Look up the raw manufacturer key + learning type by name from the keystore, so a
// clone can persist a usable kdev for SIMPLE-type mfrs (key used directly as kdev).
bool subghz_protocol_keeloq_encoder_lookup_key(
    void* context, const char* manufacture_name, uint64_t* out_key, uint8_t* out_type) {
    furi_assert(context);
    SubGhzProtocolEncoderKeeloq* instance = context;
    for
        M_EACH(mc, *subghz_keystore_get_data(instance->keystore), SubGhzKeyArray_t) {
            if(strcmp(furi_string_get_cstr(mc->name), manufacture_name) == 0) {
                *out_key = mc->key;
                *out_type = mc->type;
                return true;
            }
        }
    return false;
}
```
Header declarations in `keeloq.h`:
```c
uint64_t subghz_protocol_keeloq_encoder_last_kdev(void* context);
bool subghz_protocol_keeloq_encoder_lookup_key(
    void* context, const char* manufacture_name, uint64_t* out_key, uint8_t* out_type);
```

- [ ] **Step 2: Add the clone generator (models the existing plain generator)**

In `subghz_txrx_create_protocol_key.c`, after `subghz_txrx_gen_keeloq_protocol` (ends line 126), add:
```c
bool subghz_txrx_gen_keeloq_clone(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency,
    uint32_t serial,
    uint8_t btn,
    uint16_t cnt,
    uint16_t jump,
    const char* manufacture_name) {
    furi_assert(instance);
    bool res = false;

    instance->transmitter =
        subghz_transmitter_alloc_init(instance->environment, SUBGHZ_PROTOCOL_KEELOQ_NAME);
    subghz_txrx_set_preset(instance, preset_name, frequency, NULL, 0);

    if(instance->transmitter) {
        void* proto = subghz_transmitter_get_protocol_instance(instance->transmitter);
        // Produce a valid base frame via the normal keystore path (also runs the
        // per-mfr plaintext assembly + derivation once).
        if(subghz_protocol_keeloq_create_data(
               proto, instance->fff_data, serial, btn, cnt, manufacture_name, instance->preset)) {
            // Capture kdev: derived `man` for NORMAL/SECURE/MAGIC..., else the raw key.
            uint64_t kdev = subghz_protocol_keeloq_encoder_last_kdev(proto);
            uint8_t learn_type = 0;
            if(kdev == 0) {
                uint64_t raw = 0;
                if(subghz_protocol_keeloq_encoder_lookup_key(
                       proto, manufacture_name, &raw, &learn_type)) {
                    kdev = raw; // SIMPLE-type: key is used directly as the encrypt key
                }
            } else {
                uint64_t raw = 0;
                subghz_protocol_keeloq_encoder_lookup_key(
                    proto, manufacture_name, &raw, &learn_type);
            }

            flipper_format_write_string_cstr(instance->fff_data, "Manufacture", manufacture_name);
            uint32_t one = 1, serial_v = serial, btn_v = btn, cnt_v = cnt, jump_v = jump,
                     lt_v = learn_type, synced_v = 0;
            flipper_format_insert_or_update_uint32(instance->fff_data, "CloneMode", &one, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "Serial", &serial_v, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "Btn", &btn_v, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "Cnt", &cnt_v, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "LearnType", &lt_v, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "CloneJump", &jump_v, 1);
            flipper_format_insert_or_update_uint32(instance->fff_data, "CloneSynced", &synced_v, 1);

            uint8_t kdev_data[sizeof(uint64_t)] = {0};
            for(size_t i = 0; i < sizeof(uint64_t); i++)
                kdev_data[sizeof(uint64_t) - i - 1] = (kdev >> (i * 8)) & 0xFF;
            flipper_format_insert_or_update_hex(
                instance->fff_data, "Kdev", kdev_data, sizeof(uint64_t));
            res = true;
        }
    }
    subghz_transmitter_free(instance->transmitter);
    return res;
}
```
Add the declaration to `subghz_txrx_create_protocol_key.h` (after the plain KeeLoq decl at line 64):
```c
bool subghz_txrx_gen_keeloq_clone(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency,
    uint32_t serial,
    uint8_t btn,
    uint16_t cnt,
    uint16_t jump,
    const char* manufacture_name);
```

- [ ] **Step 3: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link.

- [ ] **Step 4: Commit**

```bash
git add lib/subghz/protocols/keeloq.c lib/subghz/protocols/keeloq.h applications/main/subghz/helpers/subghz_txrx_create_protocol_key.c applications/main/subghz/helpers/subghz_txrx_create_protocol_key.h
git commit -m "feat(keeloq): txrx no-desync clone generator (captures kdev, writes clone .sub)"
```

---

## Task 7: App-struct flags + gen_info jump + SetJump scene

Add the clone flow flag + jump storage, and a ByteInput "set jump" scene modeled on `set_counter`.

**Files:**
- Modify: `applications/main/subghz/subghz_i.h` (struct SubGhz, near line 131)
- Modify: `applications/main/subghz/helpers/subghz_gen_info.h` (keeloq gen_info arm — add `uint16_t jump;`)
- Create: `applications/main/subghz/scenes/subghz_scene_set_jump.c`
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` (register `set_jump`)

**Interfaces:**
- Consumes: `subghz_txrx_gen_keeloq_clone` (Task 6).
- Produces: `SubGhzSceneSetJump`; on completion generates the clone and routes to Transmitter.

- [ ] **Step 1: App-struct fields**

In `subghz_i.h`, after the `keeloq_mfr_emulate_flow` field (line 131) add:
```c
    // TRUE only while a KeeLoq no-desync clone is being generated (from a capture
    // or the By-Manufacturer flow), so the counter/jump path is taken instead of
    // the normal Emulate. Mirror every clear-site of keeloq_mfr_emulate_flow.
    bool keeloq_clone_flow;
    uint16_t keeloq_clone_jump;
```

- [ ] **Step 2: gen_info jump field**

In `subghz_gen_info.h`, find the keeloq gen_info arm (the struct with `serial/btn/cnt/manuf` used at `subghz->gen_info->keeloq.*`) and add `uint16_t jump;` to it.

- [ ] **Step 3: Register the scene**

In `subghz_scene_config.h`, after `ADD_SCENE(subghz, set_counter, SetCounter)` (line 26) add:
```c
ADD_SCENE(subghz, set_jump, SetJump)
```

- [ ] **Step 4: Create the SetJump scene**

Create `applications/main/subghz/scenes/subghz_scene_set_jump.c` (models `set_counter.c`'s ByteInput pattern; KeeLoq-only):
```c
#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"

#include <machine/endian.h>

#define TAG "SubGhzSetJump"

void subghz_scene_set_jump_byte_input_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventByteInputDone);
}

void subghz_scene_set_jump_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->keeloq_clone_jump == 0) subghz->keeloq_clone_jump = 100; // default lead
    subghz->keeloq_clone_jump = __bswap16(subghz->keeloq_clone_jump); // show big-endian

    ByteInput* byte_input = subghz->byte_input;
    byte_input_set_header_text(byte_input, "Enter JUMP (hex)");
    byte_input_set_result_callback(
        byte_input,
        subghz_scene_set_jump_byte_input_callback,
        NULL,
        subghz,
        (uint8_t*)&subghz->keeloq_clone_jump,
        sizeof(subghz->keeloq_clone_jump));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdByteInput);
}

bool subghz_scene_set_jump_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventByteInputDone) {
        subghz->keeloq_clone_jump = __bswap16(subghz->keeloq_clone_jump);

        bool ok = subghz_txrx_gen_keeloq_clone(
            subghz->txrx,
            subghz->gen_info->mod,
            subghz->gen_info->freq,
            subghz->gen_info->keeloq.serial,
            subghz->gen_info->keeloq.btn,
            subghz->gen_info->keeloq.cnt,
            subghz->keeloq_clone_jump,
            subghz->gen_info->keeloq.manuf);

        consumed = true;
        subghz->keeloq_clone_flow = false;
        if(!ok) {
            furi_string_set(
                subghz->error_str, "Function requires\nan SD card with\nfresh databases.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        } else {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        }
    }
    return consumed;
}

void subghz_scene_set_jump_on_exit(void* context) {
    SubGhz* subghz = context;
    byte_input_set_result_callback(subghz->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(subghz->byte_input, "");
    subghz->keeloq_clone_flow = false; // clear on any exit incl. Back
}
```

- [ ] **Step 5: Clear the new flag at the emulate-flow clear-sites**

Mirror the `keeloq_mfr_emulate_flow = false;` clears: in `subghz_scene_set_type.c` (line ~106) and `subghz_scene_keeloq_action.c` (Task 8) add `subghz->keeloq_clone_flow = false;` alongside.

- [ ] **Step 6: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link; `SubGhzSceneSetJump` resolves.

- [ ] **Step 7: Commit**

```bash
git add applications/main/subghz/subghz_i.h applications/main/subghz/helpers/subghz_gen_info.h applications/main/subghz/scenes/subghz_scene_set_jump.c applications/main/subghz/scenes/subghz_scene_config.h applications/main/subghz/scenes/subghz_scene_set_type.c
git commit -m "feat(subghz): clone-flow flags + gen_info jump + SetJump scene"
```

---

## Task 8: By-Manufacturer "Clone (no-desync)" menu item

Add a third action beside Emulate/Save. It only appears in the by-manufacturer flow (`keeloq_mfr_emulate_flow`), routes to SetJump.

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_keeloq_action.c` (full file at extraction; enum lines 5-8, on_enter 33-54, callback 10-31, on_exit 62-69)

- [ ] **Step 1: Extend the enum and callback**

Change the enum:
```c
typedef enum {
    SubGhzSceneKeeloqActionEmulate = 0,
    SubGhzSceneKeeloqActionSave = 1,
    SubGhzSceneKeeloqActionClone = 2,
} SubGhzSceneKeeloqActionIndex;
```
Add a case to `subghz_scene_keeloq_action_submenu_callback`, before `default:`:
```c
    case SubGhzSceneKeeloqActionClone:
        // No-desync clone: set the flow flag and collect the jump before generating.
        subghz->keeloq_mfr_emulate_flow = false;
        subghz->keeloq_clone_flow = true;
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetJump);
        break;
```

- [ ] **Step 2: Add the menu item**

In `subghz_scene_keeloq_action_on_enter`, after the "Save" `submenu_add_item` block, add:
```c
    submenu_add_item(
        subghz->submenu,
        "Clone (no desync)",
        SubGhzSceneKeeloqActionClone,
        subghz_scene_keeloq_action_submenu_callback,
        subghz);
```

- [ ] **Step 3: Clear the clone flag on exit**

In `subghz_scene_keeloq_action_on_exit`, add alongside the existing clear:
```c
    subghz->keeloq_clone_flow = false;
```

- [ ] **Step 4: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link.

- [ ] **Step 5: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_keeloq_action.c
git commit -m "feat(subghz): By-Manufacturer 'Clone (no desync)' action"
```

---

## Task 9: Capture -> Clone action in receiver_info

Add a "Clone" button for a decoded KeeLoq rolling capture, auto-populating serial/cnt/btn/mfr from the decoded instance (zero manual entry). Disabled for `Unknown` and SECURE (seed-only) mfrs.

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_receiver_info.c` (button add ~219-235; custom-event handler ~269-343)
- Uses: existing `subghz_scene_receiver_info_is_dynamic(subghz)` gate and the decoded parser fields.

- [ ] **Step 1: Add a `can_clone` predicate**

Near the top helpers of `subghz_scene_receiver_info.c`, add:
```c
// KeeLoq rolling captures whose learning type is encodable and whose counter was
// recovered (not Unknown) can seed a no-desync clone. Seed-only mfrs (BFT/Erreka)
// carry no seed in a normal press -> excluded.
static bool subghz_scene_receiver_info_can_clone(SubGhz* subghz) {
    const char* proto = subghz_txrx_get_protocol_name_from_history(
        subghz->txrx, subghz->history, subghz->idx_menu_chosen); // existing accessor pattern
    if(!proto || strcmp(proto, "KeeLoq") != 0) return false;
    return subghz_scene_receiver_info_is_dynamic(subghz);
}
```
(If no name accessor exists, gate on `subghz_scene_receiver_info_is_dynamic(subghz)` alone plus a KeeLoq check via the parsed decoder — confirm the exact accessor when implementing; both are present in this scene's helpers.)

- [ ] **Step 2: Add the button**

The Left slot ("ID") is currently added for dynamic captures (lines 228-235). Replace the `GuiButtonTypeLeft "ID"` add with a two-item choice, or add "Clone" on the free slot. Minimal form — add after the "ID" block:
```c
        if(subghz_scene_receiver_info_can_clone(subghz)) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeLeft,
                "Clone",
                subghz_scene_receiver_info_callback,
                subghz);
        }
```
(If Left is already taken by "ID", place "Clone" behind the ID hub as a spoke instead — see the Analyze hub pattern at lines 321-324. Decide during implementation; do not add two buttons to one slot.)

- [ ] **Step 3: Handle the event → build clone from decoded fields**

In `subghz_scene_receiver_info_on_event`, add an `else if` in the custom-event chain (after the Analyze handler at line 324):
```c
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoClone) {
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
            subghz_txrx_stop(subghz->txrx);
            if(!subghz_scene_receiver_info_update_parser(subghz)) {
                return false;
            }
            // Pull decoded serial/btn/cnt/mfr off the parser and seed gen_info.
            subghz_scene_receiver_info_fill_keeloq_gen_info(subghz); // helper below
            subghz->keeloq_clone_flow = true;
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetJump);
            return true;
```
Add `SubGhzCustomEventSceneReceiverInfoClone` to the scene's custom-event enum (same header/enum that defines `...InfoAnalyze`), and map the `GuiButtonTypeLeft`/spoke button in `subghz_scene_receiver_info_callback` to emit it.

- [ ] **Step 4: Add the gen_info fill helper**

```c
// Copy the decoded KeeLoq fields into gen_info so the clone generator can consume them.
static void subghz_scene_receiver_info_fill_keeloq_gen_info(SubGhz* subghz) {
    SubGhzBlockGeneric* g = subghz_txrx_get_decoder_generic(subghz->txrx); // existing accessor
    subghz->gen_info->type = GenKeeloq;
    subghz->gen_info->mod = "AM650";
    subghz->gen_info->freq = 433920000;
    subghz->gen_info->keeloq.serial = g->serial;
    subghz->gen_info->keeloq.btn = g->btn;
    subghz->gen_info->keeloq.cnt = (uint16_t)g->cnt;
    subghz->gen_info->keeloq.manuf = subghz_txrx_get_decoder_manufacture_name(subghz->txrx);
}
```
Confirm the exact accessor names for the decoder's generic block + manufacture name during implementation (the scene already reads decoded fields for its Fix/Cnt/MF display — reuse those accessors). If the frequency/preset of the capture is known from history, prefer those over the 433.92 default.

- [ ] **Step 5: Compile-gate**

Run: `build_ci.bat`.
Expected: clean link.

- [ ] **Step 6: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_receiver_info.c
git commit -m "feat(subghz): capture -> no-desync clone action"
```

---

## Task 10: Mirror to cardputer_adv + build both trees

**Files:**
- Copy all created files and re-apply all modifications into `C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_cardputer_adv/` at the mirror paths.

- [ ] **Step 1: Copy the two new core files + test + scene verbatim** into the cardputer tree (identical relative paths). Re-apply the keeloq.c / scene / helper / subghz_i.h edits (the files are byte-parallel across trees per the repo's cross-tree discipline).

- [ ] **Step 2: Host test in the cardputer tree**

Run: `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_cardputer_adv && gcc -Wall -Wextra -o /tmp/t tests/test_keeloq_clone_core.c && /tmp/t"`
Expected: PASS.

- [ ] **Step 3: Build m5stick tree**

Run: `build_ci.bat` in `src_m5stick_s3`. Expected: success.

- [ ] **Step 4: Build cardputer tree**

Run: `build_cardputer_adv` build (per the tree's build script). Expected: success. (Build ONE env at a time.)

- [ ] **Step 5: Public-guard check**

Confirm all new symbols compile out (or are inert) under `#ifndef PUBLIC_BUILD` — the clone menu items / capture action must not appear in public/hardened. Build the public env and confirm clean link with no clone UI.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(keeloq): mirror no-desync clone to cardputer_adv; build both trees"
```

---

## Task 11: On-hardware verification (manual)

Flash m5stick (COM10) / cardputer (COM12). No code; a checklist that must pass before calling the feature done.

- [ ] **Clone-from-capture:** capture a NORMAL-learning fob press → "Clone" → set jump (default 100) → Transmitter → target device actuates. Confirm serial/cnt auto-filled (no manual entry).
- [ ] **No-desync proof:** press the REAL fob several times (advance its counter), then transmit the clone → device still actuates (clone jumped ahead; the pair resynced).
- [ ] **Resume:** save the clone, re-open it → transmit → still actuates, counter advanced (+2 per prior send).
- [ ] **By-Manufacturer path:** KL: By Manufacturer → serial/btn/counter → "Clone (no desync)" → set jump → transmit. Emulate/Save unchanged.
- [ ] **Guards:** an `Unknown` capture and a BFT (SECURE) capture do NOT offer Clone; normal Emulate/Save behave exactly as before; public build shows no clone UI.
- [ ] Record results in `BUGFIXES.md` if any on-hardware issue is found; otherwise note verification pass.

---

## Self-Review

**Spec coverage:** A (TX engine) → Tasks 2,3; B (persistence) → Tasks 5,6; C (entry points) → Tasks 7,8,9; counter algorithm (jump + pair + synced +2) → Task 1; store-Kdev decision → Tasks 2,6; private-only → Global Constraints + Task 10; edge cases (Unknown/SECURE disabled, wrap, per-mfr assembly kept) → Tasks 1,2,9 + Global Constraints. Covered.

**Placeholder scan:** Tasks 9's accessor names (`subghz_txrx_get_decoder_generic` / `_manufacture_name` / protocol-name accessor) are marked "confirm exact name during implementation" because the receiver_info helper set was not fully extracted; the scene already reads these decoded fields for its Fix/Cnt/MF display, so the accessors exist — the implementer reuses them. This is the one bounded lookup, not a logic gap.

**Type consistency:** `KeeloqClone` fields and `keeloq_clone_next_pair` signature match across Tasks 1/2/3/5. `subghz_txrx_gen_keeloq_clone` signature matches between Task 6 (def) and Task 7 (call). `.sub` field names (`CloneMode/Serial/Btn/Cnt/Kdev/LearnType/CloneJump/CloneSynced`) match between Task 6 (write) and Task 5 (read).
