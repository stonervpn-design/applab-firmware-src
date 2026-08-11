# Sub-GHz Protocol Fuzzer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a private-only Sub-GHz Protocol Fuzzer that transmits mutated/malformed frames (RAW + fixed-code protocol modes) to probe a receiver's parser, reusing the existing CC1101 TX path.

**Architecture:** Pure, host-tested mutation + frame-serialization core (`fuzzer_mutate`, `fuzzer_frame`) drives a `fuzzer_worker` cloned from `subbrute_worker` that builds a `FlipperFormat` and transmits via the existing `subghz_txrx_tx_start` async-TX path. A config scene + run scene/view expose it under the SubGHz menu. Everything is `#ifndef PUBLIC_BUILD`.

**Tech Stack:** C (ESP-IDF firmware), Flipper port compat layer (`furi`, `gui`, `flipper_format`, `subghz_devices`), host unit tests in C compiled with WSL gcc.

## Global Constraints

- **Private-only:** every new source's app-visible entry (app/scene/menu/worker alloc + free) MUST be guarded `#ifndef PUBLIC_BUILD`. Public ELF must contain **0** fuzzer symbols after gc. (Copy the pattern used for the jammer/rollback.)
- **No new radio code:** transmit only through the existing path — build a `FlipperFormat` and call the same deserialize→`subghz_devices_start_async_tx` sequence `subbrute_worker_subghz_transmit` uses, or `subghz_txrx_tx_start`.
- **Frequency allowlist:** the fuzzer MUST validate the chosen frequency through the existing SubGHz TX frequency check before transmitting (same guard the Transmitter/SubBrute use); never TX out of band.
- **Build via PowerShell** (`& "...\build_m5stick_s3.bat"`), NOT the Bash tool (MSYS mangles cmd args → phantom build). When files are ADDED, the CMake globs are cached — do a clean reconfigure (delete the build dir) on the first build after adding sources.
- **Host tests** compile+run under WSL gcc: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra tests/<t>.c -o /tmp/<t> && /tmp/<t> && echo TESTS_PASS"`. Tests `#include` the module `.c` directly and assert in `main()` (mirror `tests/test_subghz_decode_dedup.c`).
- **Commit** after each task with `-c core.autocrlf=false`; footer as elsewhere. No push.
- Board: build/verify on **m5stick_s3** private; the tool also compiles for cardputer_adv private (input mapping differs — see Task 8).

---

### Task 1: `fuzzer_mutate` core — types + bit-flip sweep

**Files:**
- Create: `applications/main/subghz/helpers/fuzzer_mutate.h`
- Create: `applications/main/subghz/helpers/fuzzer_mutate.c`
- Test: `tests/test_fuzzer_mutate.c`

**Interfaces:**
- Produces: the `FuzzMode`, `FuzzStrategy`, `FuzzConfig`, `FuzzFrame` types; `uint32_t fuzzer_mutate_count(const FuzzConfig*)`; `bool fuzzer_mutate_frame(const FuzzConfig*, uint32_t index, FuzzFrame* out)`. Later tasks (frame builder, worker) consume these.

- [ ] **Step 1: Write the failing test** (`tests/test_fuzzer_mutate.c`)

```c
// tests/test_fuzzer_mutate.c
#include "../applications/main/subghz/helpers/fuzzer_mutate.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FuzzConfig proto_cfg(FuzzStrategy s) {
    FuzzConfig c; memset(&c, 0, sizeof c);
    c.mode = FuzzModeProtocol; c.strategy = s; c.seed = 1;
    c.seed_nbits = 24;                 // 3-byte seed 0xAA 0x55 0x0F
    c.seed_bits[0] = 0xAA; c.seed_bits[1] = 0x55; c.seed_bits[2] = 0x0F;
    return c;
}

static void test_bitflip_sweep(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyBitFlipSweep);
    // one frame per bit
    assert(fuzzer_mutate_count(&c) == 24);
    FuzzFrame f;
    // index 0 flips bit 0 (MSB of byte 0): 0xAA -> 0x2A
    assert(fuzzer_mutate_frame(&c, 0, &f));
    assert(f.nbits == 24);
    assert(f.bits[0] == 0x2A && f.bits[1] == 0x55 && f.bits[2] == 0x0F);
    assert(f.index == 0 && f.strategy == FuzzStrategyBitFlipSweep);
    // index 23 flips last bit: 0x0F -> 0x0E
    assert(fuzzer_mutate_frame(&c, 23, &f));
    assert(f.bits[2] == 0x0E && f.bits[0] == 0xAA);
    // out of range
    assert(!fuzzer_mutate_frame(&c, 24, &f));
    printf("test_bitflip_sweep OK\n");
}

int main(void) {
    test_bitflip_sweep();
    printf("TESTS_PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_fuzzer_mutate.c -o /tmp/tfm 2>&1 | head"`
Expected: FAIL — `fuzzer_mutate.c: No such file` / undefined types.

- [ ] **Step 3: Write minimal header + implementation**

`fuzzer_mutate.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FUZZ_MAX_BYTES 16    // 128-bit max frame (protocol)
#define FUZZ_MAX_DUR   256   // raw durations

typedef enum { FuzzModeRaw, FuzzModeProtocol } FuzzMode;
typedef enum {
    FuzzStrategyBitFlipSweep,
    FuzzStrategyBoundary,
    FuzzStrategyLength,
    FuzzStrategyRandom,
    FuzzStrategyFieldWalk,
} FuzzStrategy;

typedef struct {
    FuzzMode mode;
    FuzzStrategy strategy;
    uint32_t seed;
    uint8_t  seed_bits[FUZZ_MAX_BYTES];
    uint16_t seed_nbits;
    int16_t  seed_dur[FUZZ_MAX_DUR];   // +pulse / -gap
    uint16_t seed_ndur;
    uint16_t field_off, field_len;     // FieldWalk (bits)
    int16_t  len_min_delta, len_max_delta; // Length (bits or durations)
    uint8_t  rand_k;                   // Random: corruptions/frame
    uint32_t rand_iters;               // Random: frame count
} FuzzConfig;

typedef struct {
    uint8_t  bits[FUZZ_MAX_BYTES];
    uint16_t nbits;
    int16_t  dur[FUZZ_MAX_DUR];
    uint16_t ndur;
    FuzzStrategy strategy;
    uint32_t seed;
    uint32_t index;
} FuzzFrame;

// Number of frames for deterministic strategies; rand_iters for Random.
uint32_t fuzzer_mutate_count(const FuzzConfig* cfg);
// Fill out with the frame at index. false if index >= count.
bool fuzzer_mutate_frame(const FuzzConfig* cfg, uint32_t index, FuzzFrame* out);
```

`fuzzer_mutate.c` (only bit-flip sweep for now):
```c
#include "fuzzer_mutate.h"
#include <string.h>

static void seed_into(const FuzzConfig* c, FuzzFrame* o) {
    memset(o, 0, sizeof(*o));
    o->strategy = c->strategy; o->seed = c->seed;
    if(c->mode == FuzzModeProtocol) {
        o->nbits = c->seed_nbits;
        memcpy(o->bits, c->seed_bits, (c->seed_nbits + 7) / 8);
    } else {
        o->ndur = c->seed_ndur;
        memcpy(o->dur, c->seed_dur, c->seed_ndur * sizeof(int16_t));
    }
}

uint32_t fuzzer_mutate_count(const FuzzConfig* c) {
    switch(c->strategy) {
    case FuzzStrategyBitFlipSweep:
        return (c->mode == FuzzModeProtocol) ? c->seed_nbits : (uint32_t)c->seed_ndur * 16;
    default:
        return 0;
    }
}

bool fuzzer_mutate_frame(const FuzzConfig* c, uint32_t index, FuzzFrame* o) {
    if(index >= fuzzer_mutate_count(c)) return false;
    seed_into(c, o);
    o->index = index;
    if(c->strategy == FuzzStrategyBitFlipSweep && c->mode == FuzzModeProtocol) {
        o->bits[index / 8] ^= (uint8_t)(0x80u >> (index % 8)); // bit 0 = MSB
        return true;
    }
    return true; // raw bit-flip handled in Task 2 extension
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_fuzzer_mutate.c -o /tmp/tfm && /tmp/tfm"`
Expected: `test_bitflip_sweep OK` then `TESTS_PASS`.

- [ ] **Step 5: Commit**

```bash
git add applications/main/subghz/helpers/fuzzer_mutate.h applications/main/subghz/helpers/fuzzer_mutate.c tests/test_fuzzer_mutate.c
git commit -c core.autocrlf=false -m "feat(fuzzer): mutation core + bit-flip sweep (host-tested)"
```

---

### Task 2: `fuzzer_mutate` — boundary + length strategies

**Files:**
- Modify: `applications/main/subghz/helpers/fuzzer_mutate.c`
- Test: `tests/test_fuzzer_mutate.c` (add cases)

**Interfaces:**
- Consumes: Task 1 types/functions. Produces: `FuzzStrategyBoundary` (fixed 4-frame set: all-0, all-1, nbits-1, nbits+1) and `FuzzStrategyLength` (nbits from seed_nbits+len_min_delta..+len_max_delta) behavior in `fuzzer_mutate_count` / `_frame`.

- [ ] **Step 1: Add failing tests**

```c
static void test_boundary(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyBoundary);
    assert(fuzzer_mutate_count(&c) == 4);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); // all zeros, same nbits
    assert(f.nbits == 24 && f.bits[0] == 0 && f.bits[1] == 0 && f.bits[2] == 0);
    fuzzer_mutate_frame(&c, 1, &f); // all ones
    assert(f.bits[0] == 0xFF && f.bits[1] == 0xFF && f.bits[2] == 0xFF);
    fuzzer_mutate_frame(&c, 2, &f); assert(f.nbits == 23); // nbits-1
    fuzzer_mutate_frame(&c, 3, &f); assert(f.nbits == 25); // nbits+1
    printf("test_boundary OK\n");
}
static void test_length(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyLength);
    c.len_min_delta = -2; c.len_max_delta = 2;      // -2..+2 => 5 frames
    assert(fuzzer_mutate_count(&c) == 5);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); assert(f.nbits == 22);
    fuzzer_mutate_frame(&c, 4, &f); assert(f.nbits == 26);
    printf("test_length OK\n");
}
```
Call both in `main()` before the final print.

- [ ] **Step 2: Run — expect FAIL** (`count`/`frame` don't handle new strategies).
Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_fuzzer_mutate.c -o /tmp/tfm && /tmp/tfm"`

- [ ] **Step 3: Implement** — extend `fuzzer_mutate_count` and `fuzzer_mutate_frame`:
```c
// in fuzzer_mutate_count switch:
case FuzzStrategyBoundary: return 4;
case FuzzStrategyLength:   return (uint32_t)(c->len_max_delta - c->len_min_delta + 1);
// in fuzzer_mutate_frame, after seed_into/index set, protocol mode:
if(c->strategy == FuzzStrategyBoundary) {
    uint16_t nb = c->seed_nbits;
    switch(index) {
    case 0: memset(o->bits, 0x00, FUZZ_MAX_BYTES); break;
    case 1: memset(o->bits, 0xFF, FUZZ_MAX_BYTES); break;
    case 2: o->nbits = nb ? nb - 1 : 0; break;
    case 3: o->nbits = nb + 1; break;
    }
    return true;
}
if(c->strategy == FuzzStrategyLength) {
    o->nbits = (uint16_t)((int)c->seed_nbits + c->len_min_delta + (int)index);
    return true;
}
```

- [ ] **Step 4: Run — expect PASS** (`test_boundary OK`, `test_length OK`, `TESTS_PASS`).

- [ ] **Step 5: Commit**
```bash
git add applications/main/subghz/helpers/fuzzer_mutate.c tests/test_fuzzer_mutate.c
git commit -c core.autocrlf=false -m "feat(fuzzer): boundary + length mutation strategies (host-tested)"
```

---

### Task 3: `fuzzer_mutate` — seeded random + field-targeted walk

**Files:**
- Modify: `applications/main/subghz/helpers/fuzzer_mutate.c`
- Test: `tests/test_fuzzer_mutate.c` (add cases)

**Interfaces:**
- Consumes: Task 1/2. Produces: `FuzzStrategyRandom` (count = `rand_iters`, deterministic per `(seed,index)` via an internal splitmix32 PRNG `fuzz_rng(seed ^ index)`), `FuzzStrategyFieldWalk` (count = `2^field_len` capped, walks the field bits `field_off..field_off+field_len`).

- [ ] **Step 1: Add failing tests**
```c
static void test_random_reproducible(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyRandom);
    c.rand_iters = 50; c.rand_k = 3;
    assert(fuzzer_mutate_count(&c) == 50);
    FuzzFrame a, b;
    assert(fuzzer_mutate_frame(&c, 7, &a));
    assert(fuzzer_mutate_frame(&c, 7, &b));       // same index -> identical
    assert(memcmp(a.bits, b.bits, 3) == 0);
    FuzzFrame d; fuzzer_mutate_frame(&c, 8, &d);   // different index -> differs (very likely)
    assert(memcmp(a.bits, d.bits, 3) != 0 || a.nbits != d.nbits);
    printf("test_random_reproducible OK\n");
}
static void test_field_walk(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyFieldWalk);
    c.field_off = 20; c.field_len = 4;            // low nibble of byte 2
    assert(fuzzer_mutate_count(&c) == 16);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); assert((f.bits[2] & 0x0F) == 0x0);
    fuzzer_mutate_frame(&c, 5, &f); assert((f.bits[2] & 0x0F) == 0x5);
    fuzzer_mutate_frame(&c, 15, &f); assert((f.bits[2] & 0x0F) == 0xF);
    printf("test_field_walk OK\n");
}
```
Call both in `main()`.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement** — add PRNG + strategies:
```c
static uint32_t fuzz_rng(uint32_t x) { // splitmix32
    x += 0x9E3779B9u; x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u; return x ^ (x >> 16);
}
// count:
case FuzzStrategyRandom: return c->rand_iters;
case FuzzStrategyFieldWalk: {
    uint32_t n = (c->field_len >= 20) ? (1u << 20) : (1u << c->field_len);
    return n;
}
// frame (protocol mode), after seed_into/index:
if(c->strategy == FuzzStrategyRandom) {
    uint32_t r = fuzz_rng(c->seed ^ index);
    uint16_t nbytes = (c->seed_nbits + 7) / 8;
    for(uint8_t i = 0; i < c->rand_k && nbytes; i++) {
        r = fuzz_rng(r);
        o->bits[r % nbytes] ^= (uint8_t)(r >> 8);
    }
    return true;
}
if(c->strategy == FuzzStrategyFieldWalk) {
    // write `index` into field_len bits at field_off (MSB-first)
    for(uint16_t i = 0; i < c->field_len; i++) {
        uint16_t bitpos = c->field_off + i;
        uint8_t bitval = (index >> (c->field_len - 1 - i)) & 1u;
        uint8_t* B = &o->bits[bitpos / 8];
        uint8_t m = (uint8_t)(0x80u >> (bitpos % 8));
        if(bitval) *B |= m; else *B &= (uint8_t)~m;
    }
    return true;
}
```

- [ ] **Step 4: Run — expect PASS** (both new lines + `TESTS_PASS`).

- [ ] **Step 5: Commit**
```bash
git add applications/main/subghz/helpers/fuzzer_mutate.c tests/test_fuzzer_mutate.c
git commit -c core.autocrlf=false -m "feat(fuzzer): seeded-random + field-walk mutations (host-tested)"
```

---

### Task 4: `fuzzer_frame` — FlipperFormat text builder (RAW + protocol)

**Files:**
- Create: `applications/main/subghz/helpers/fuzzer_frame.h`
- Create: `applications/main/subghz/helpers/fuzzer_frame.c`
- Test: `tests/test_fuzzer_frame.c`

**Interfaces:**
- Consumes: `FuzzFrame` (Task 1). Produces: `int fuzzer_frame_build(const FuzzFrame* f, FuzzMode mode, const char* protocol, uint32_t te, uint8_t repeat, char* out, int cap)` — writes the `flipper_format` text body the worker will load into a `FlipperFormat`, returns bytes written (or -1 on overflow). Pure (only `snprintf`), host-testable.

- [ ] **Step 1: Failing test** (`tests/test_fuzzer_frame.c`)
```c
#include "../applications/main/subghz/helpers/fuzzer_frame.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    FuzzFrame f; memset(&f, 0, sizeof f);
    f.nbits = 24; f.bits[0]=0xAA; f.bits[1]=0x55; f.bits[2]=0x0F;
    char buf[512];
    int n = fuzzer_frame_build(&f, FuzzModeProtocol, "Princeton", 400, 5, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "Protocol: Princeton"));
    assert(strstr(buf, "Bit: 24"));
    assert(strstr(buf, "Key: 00 00 00 00 00 AA 55 0F")); // right-justified 8 bytes
    assert(strstr(buf, "TE: 400"));
    // raw mode
    FuzzFrame r; memset(&r, 0, sizeof r); r.ndur = 4;
    r.dur[0]=400; r.dur[1]=-400; r.dur[2]=400; r.dur[3]=-1200;
    n = fuzzer_frame_build(&r, FuzzModeRaw, NULL, 0, 1, buf, sizeof buf);
    assert(n > 0 && strstr(buf, "Protocol: RAW") && strstr(buf, "RAW_Data: 400 -400 400 -1200"));
    printf("TESTS_PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run — expect FAIL** (`fuzzer_frame.c` missing).
Run: `wsl bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall tests/test_fuzzer_frame.c -o /tmp/tff 2>&1 | head"`

- [ ] **Step 3: Implement** (`fuzzer_frame.h` + `.c`)
`fuzzer_frame.h`:
```c
#pragma once
#include "fuzzer_mutate.h"
int fuzzer_frame_build(const FuzzFrame* f, FuzzMode mode, const char* protocol,
                       uint32_t te, uint8_t repeat, char* out, int cap);
```
`fuzzer_frame.c`:
```c
#include "fuzzer_frame.h"
#include <stdio.h>
#include <string.h>

int fuzzer_frame_build(const FuzzFrame* f, FuzzMode mode, const char* protocol,
                       uint32_t te, uint8_t repeat, char* out, int cap) {
    int n = 0;
    if(mode == FuzzModeProtocol) {
        // Key is 8 bytes, right-justified: pad high bytes with 00.
        uint8_t key[8]; memset(key, 0, sizeof key);
        int nb = (f->nbits + 7) / 8; if(nb > 8) nb = 8;
        for(int i = 0; i < nb; i++) key[8 - nb + i] = f->bits[i];
        n = snprintf(out, cap,
            "Protocol: %s\nBit: %u\nKey: %02X %02X %02X %02X %02X %02X %02X %02X\nTE: %lu\nRepeat: %u\n",
            protocol ? protocol : "Princeton", (unsigned)f->nbits,
            key[0],key[1],key[2],key[3],key[4],key[5],key[6],key[7],
            (unsigned long)te, (unsigned)repeat);
    } else {
        n = snprintf(out, cap, "Protocol: RAW\nRAW_Data:");
        for(uint16_t i = 0; i < f->ndur && n > 0 && n < cap; i++)
            n += snprintf(out + n, cap - n, " %d", (int)f->dur[i]);
        if(n > 0 && n < cap) n += snprintf(out + n, cap - n, "\n");
    }
    return (n > 0 && n < cap) ? n : -1;
}
```

- [ ] **Step 4: Run — expect PASS** (`TESTS_PASS`).

- [ ] **Step 5: Commit**
```bash
git add applications/main/subghz/helpers/fuzzer_frame.h applications/main/subghz/helpers/fuzzer_frame.c tests/test_fuzzer_frame.c
git commit -c core.autocrlf=false -m "feat(fuzzer): flipper_format text builder RAW+protocol (host-tested)"
```

---

### Task 5: `fuzzer_worker` — TX loop (firmware, compile-verified)

**Files:**
- Create: `applications/main/subghz/helpers/fuzzer_worker.h`
- Create: `applications/main/subghz/helpers/fuzzer_worker.c`
- Reference: `applications/main/subghz/helpers/subbrute_worker.c` (clone start/stop/thread/`subbrute_worker_subghz_transmit`), `subghz_txrx.c:453 subghz_txrx_tx_start`.

**Interfaces:**
- Consumes: `fuzzer_mutate_*`, `fuzzer_frame_build`, the SubGHz environment/txrx handle held by the SubGhz app.
- Produces: `FuzzerWorker* fuzzer_worker_alloc(void)`, `_free`, `_set_config(FuzzerWorker*, const FuzzConfig*, const char* protocol, uint32_t freq, const char* preset, uint32_t te, uint8_t repeat)`, `_start/_stop/_pause/_step`, `bool fuzzer_worker_running(...)`, `uint32_t fuzzer_worker_index(...)`, and a callback `_set_callback(worker, cb, ctx)` fired per frame with the current index/total. Also `bool fuzzer_worker_mark_last(FuzzerWorker*, const char* path)` — writes the last-sent frame to `path` as a `.sub`.

- [ ] **Step 1: Write the worker** — clone the `subbrute_worker` thread skeleton. Per tick: `fuzzer_mutate_frame(cfg, index, &frame)` → `fuzzer_frame_build(...)` into a char buf → `flipper_format_string_alloc()` + `flipper_format_write_string_cstr`/raw-stream write of the buf → validate frequency via the existing SubGHz freq check → transmit with the same deserialize→`subghz_devices_start_async_tx` sequence as `subbrute_worker_subghz_transmit`. Advance `index` (auto) or wait for `_step`. Guard the whole file body so it is compiled but inert if ever built public is unnecessary — it is only referenced by guarded scenes, so it gc's; no in-file guard needed, but the alloc/free call sites in Task 8/9 ARE guarded.

- [ ] **Step 2: Add to build & compile** — new files are picked up by the component glob; first build needs a clean reconfigure. In PowerShell:
```
Remove-Item -Recurse -Force build_m5stick_s3 -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Expected: build log ends `SENTINEL_OK` (private build). Verify the log mtime advanced and `nm build_m5stick_s3/furi_esp32.elf | grep fuzzer_worker` shows the symbols.

- [ ] **Step 3: Commit**
```bash
git add applications/main/subghz/helpers/fuzzer_worker.h applications/main/subghz/helpers/fuzzer_worker.c
git commit -c core.autocrlf=false -m "feat(fuzzer): TX worker cloned from subbrute (compiles, private)"
```

---

### Task 6: `subghz_view_fuzzer` — run view (firmware, compile-verified)

**Files:**
- Create: `applications/main/subghz/views/subghz_view_fuzzer.h`
- Create: `applications/main/subghz/views/subghz_view_fuzzer.c`
- Reference: an existing simple view, e.g. `applications/main/subghz/views/subghz_view_spectrum.c`, for the `View*` alloc/draw/input pattern.

**Interfaces:**
- Produces: `SubGhzViewFuzzer* subghz_view_fuzzer_alloc(void)`, `_free`, `View* subghz_view_fuzzer_get_view(...)`, `_set_data(view, freq, mode_str, mutation_str, index, total, tx_on)`, and an input callback hook `_set_callback(view, cb, ctx)` emitting events: Start/Pause, Step, Mark, Stop.

- [ ] **Step 1: Write the view** — draw: line 1 freq + preset; line 2 mode + protocol; line 3 `frame N/TOTAL`; line 4 mutation descriptor; a small TX blink box; footer hint of the control keys. Model the draw/input plumbing on `subghz_view_spectrum.c`.

- [ ] **Step 2: Compile** (incremental ok now that globs are configured):
```
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\inc_build_m5stick.bat"
```
Expected: `SENTINEL_OK`; `nm ... | grep subghz_view_fuzzer` present.

- [ ] **Step 3: Commit**
```bash
git add applications/main/subghz/views/subghz_view_fuzzer.h applications/main/subghz/views/subghz_view_fuzzer.c
git commit -c core.autocrlf=false -m "feat(fuzzer): run view (compiles, private)"
```

---

### Task 7: Config scene (firmware, compile-verified)

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_config.c`
- Reference: an existing `VariableItemList` config scene (e.g. a brute/jammer config scene) for the pattern.

**Interfaces:**
- Consumes: `FuzzConfig`, the SubGhz app context. Produces: the scene body `subghz_scene_protocol_fuzzer_config_on_{enter,event,exit}` that fills a `FuzzConfig` on the app context and transitions to the run scene on confirm.

- [ ] **Step 1: Write the scene** — VariableItemList items: Mode (RAW/Protocol), Protocol (Princeton/CAME/NICE FLO/Holtek/Linear), Frequency, Preset (OOK650/AM270/FM238…), Seed (Default/Load .sub), Strategy (BitFlip/Boundary/Length/Random/FieldWalk), Rate, Repeat, Run (Auto/Step). Store selections into `app->fuzzer_cfg`. "Start" confirm → `scene_manager_next_scene(ProtocolFuzzerRun)`.

- [ ] **Step 2: Compile** `& inc_build_m5stick.bat` → `SENTINEL_OK`.

- [ ] **Step 3: Commit**
```bash
git add applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_config.c
git commit -c core.autocrlf=false -m "feat(fuzzer): config scene (compiles, private)"
```

---

### Task 8: Run scene — wire worker+view, input map, Mark-to-.sub (firmware)

**Files:**
- Create: `applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_run.c`
- Reference: how a scene owns a worker+view and routes input (e.g. the jammer or brute run scene).

**Interfaces:**
- Consumes: `fuzzer_worker_*`, `subghz_view_fuzzer_*`, `app->fuzzer_cfg`. Produces: the run scene that allocs the worker, sets the view as current, maps input → worker, and on Mark calls `fuzzer_worker_mark_last(worker, "/ext/subghz/fuzzer/mark_<idx>.sub")` (creating the dir if absent).

- [ ] **Step 1: Write the scene** — on_enter: alloc worker, `fuzzer_worker_set_config(...)` from `app->fuzzer_cfg`, set view callback. Input mapping per board: **Cardputer** Space=Start/Pause, →=Step, M=Mark, Esc=Stop; **m5stick** (`BOARD_TWO_BUTTON_INPUT`) tap=Step, hold KEY1=Start/Pause, KEY2=Back/Stop, and auto-save-on-stop for Mark. on_exit: stop+free worker.

- [ ] **Step 2: Compile** `& inc_build_m5stick.bat` → `SENTINEL_OK`. (Menu not wired yet; scene compiles as an island referenced only after Task 9.)

- [ ] **Step 3: Commit**
```bash
git add applications/main/subghz/scenes/subghz_scene_protocol_fuzzer_run.c
git commit -c core.autocrlf=false -m "feat(fuzzer): run scene + mark-to-.sub (compiles, private)"
```

---

### Task 9: Menu + scene registration wiring (all `#ifndef PUBLIC_BUILD`)

**Files:**
- Modify: `applications/main/subghz/subghz_i.h` — add `SubmenuIndexProtocolFuzzer` to the submenu-index enum and, if the view uses a fixed id, `SubGhzViewIdFuzzer`.
- Modify: `applications/main/subghz/scenes/subghz_scene_config.h` — `ADD_SCENE(subghz, protocol_fuzzer_config, ProtocolFuzzerConfig)` and `ADD_SCENE(subghz, protocol_fuzzer_run, ProtocolFuzzerRun)`, both wrapped `#ifndef PUBLIC_BUILD`.
- Modify: `applications/main/subghz/scenes/subghz_scene_start.c` — add the `submenu_add_item(... "Protocol Fuzzer", SubmenuIndexProtocolFuzzer ...)` and its handler branch (`scene_manager_next_scene(SubGhzSceneProtocolFuzzerConfig)`), both `#ifndef PUBLIC_BUILD` (mirror the guarded Jammer/RollBack items at lines 45–76).

**Interfaces:**
- Consumes: scenes from Tasks 7–8. Produces: a reachable "Protocol Fuzzer" menu entry (private only).

- [ ] **Step 1: Make the edits** exactly as above, each new block guarded.

- [ ] **Step 2: Clean reconfigure build** (scene_config.h + enum changed):
```
Remove-Item -Recurse -Force build_m5stick_s3 -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Expected: `SENTINEL_OK`. Sanity: `nm build_m5stick_s3/furi_esp32.elf | grep -E "fuzzer_worker|subghz_scene_protocol_fuzzer"` present.

- [ ] **Step 3: Commit**
```bash
git add applications/main/subghz/subghz_i.h applications/main/subghz/scenes/subghz_scene_config.h applications/main/subghz/scenes/subghz_scene_start.c
git commit -c core.autocrlf=false -m "feat(fuzzer): menu + scene registration, private-gated"
```

---

### Task 10: Public-build gate — verify 0 fuzzer symbols on public

**Files:** none (verification only).

- [ ] **Step 1: Build public** (m5stick), reliable path:
```
Remove-Item -Recurse -Force build_m5stick_s3_public -ErrorAction SilentlyContinue
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3_public.bat"
```
Expected: `SENTINEL_OK`.

- [ ] **Step 2: Assert no fuzzer symbols + car-free gate PASS**
```
$nm = (Get-ChildItem "$env:USERPROFILE\.espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe").FullName
& $nm build_m5stick_s3_public\furi_esp32.elf | Select-String "fuzzer_worker|subghz_scene_protocol_fuzzer|subghz_view_fuzzer"
python tools\verify_public_no_car.py build_m5stick_s3_public\furi_esp32.bin
```
Expected: the Select-String returns **nothing** (0 fuzzer symbols, gc'd), and the car gate prints PASS.

- [ ] **Step 3: Commit** (record the verification in a note if desired; otherwise nothing to commit). Optionally flash the private m5stick build (`& flash_m5stick_com10.bat write`) for on-device smoke per spec §9.

---

## Self-Review

**Spec coverage:** §2 modes → Tasks 1–4/5/7; §3 mutations → Tasks 1–3; §4 worker/delivery → Task 5; §5 reproducibility (index/step/Mark) → Tasks 5(mark)/6(view)/8(run); §6 UI/scenes/input → Tasks 6–9; §7 gating+freq allowlist → Task 5 (freq check) + Task 9 (guards) + Task 10 (verify); §8 MVP protocol set → Task 7; §9 testing → host tests Tasks 1–4 + Task 10 gate + on-device smoke note. All covered.

**Placeholder scan:** host-test tasks contain full test + impl code. Firmware tasks (5–9) give exact files, interfaces, references to confirmed existing patterns, and real build/verify commands; they are compile-gated because they can't be host-tested.

**Type consistency:** `FuzzConfig`/`FuzzFrame`/`FuzzStrategy`/`FuzzMode` defined in Task 1, consumed unchanged in 2–5; `fuzzer_frame_build` signature identical in Task 4 header/impl/consumer (Task 5); worker function names (`fuzzer_worker_*`) consistent across Tasks 5/8/9.
