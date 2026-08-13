# BadUSB OS-Conditional Autorun (USB1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `IF_OS <WINDOWS|MAC|LINUX> … [ELSE_OS] … END_IF` (+ `SET_OS`) conditional blocks to the DuckyScript interpreter, with an operator-selected target OS, so one payload branches per host OS. PRIVATE-gated (bad_usb is a public app).

**Architecture:** A pure, host-tested block/skip state machine (`ducky_os_cond`) is consulted by the interpreter's per-line loop; directive lines update state, non-matching-branch lines are skipped. A config-scene "Target OS" selector sets the initial OS. Spec: `docs/superpowers/specs/2026-08-13-badusb-os-conditional-design.md`.

**Tech Stack:** C (furi), WSL gcc host tests. Branch `feat/redteam-gapfill`.

## Global Constraints
- **PRIVATE-gated.** `bad_usb` ships in PUBLIC builds (`fam_config.py` APPS, no exclusion). So ALL USB1 additions are `#ifndef PUBLIC_BUILD`: the whole `ducky_os_cond.{c}` body, the new `ducky_commands[]` entries + handlers, the `DuckyOsCondState` field on `BadUsbScript`, the per-line eval call, and the config-scene "Target OS" row. Public build must not recognize `IF_OS`/`SET_OS` and must contain **0** new USB1 symbols (nm-scan proof). Precedent for per-feature gating inside a mixed file: the subghz Protocol Fuzzer (`subghz_scene_config.h` / `subghz_scene_start.c`).
- **Operator-selected OS only** — NO LED/auto-detect in v1. The reliable target is the config selector + `SET_OS`.
- **Robustness:** malformed directive → defined no-op/error (never a crash or silent wrong-branch); nesting depth bounded (≤8); unbalanced `ELSE_OS`/`END_IF` handled gracefully.
- **Fresh-log build method** (delete dir → absolute-path `.bat` → verify fresh log mtime + SENTINEL + obj recompiled). Private: `build_m5stick_s3.bat`, `build_adv_only_private.bat`. Public exclusion: `build_m5stick_s3_public.bat`.

---

### Task 1: Conditional state machine `ducky_os_cond.{c,h}` + host test

**Files:**
- Create: `applications/main/bad_usb/helpers/ducky_os_cond.h` (furi-free leaf)
- Create: `applications/main/bad_usb/helpers/ducky_os_cond.c` (whole body `#ifndef PUBLIC_BUILD`)
- Test: `tests/test_ducky_os_cond.c`

**Interfaces:**
- Produces:
  ```c
  typedef enum { DUCKY_OS_WINDOWS = 0, DUCKY_OS_MAC = 1, DUCKY_OS_LINUX = 2 } DuckyOs;
  typedef enum {
      DUCKY_COND_EXECUTE,   /* run this (non-directive) line normally */
      DUCKY_COND_SKIP,      /* inside a non-matching branch: no-op this line */
      DUCKY_COND_DIRECTIVE, /* line was IF_OS/ELSE_OS/END_IF/SET_OS, consumed */
      DUCKY_COND_ERROR      /* malformed directive / unbalanced / overflow */
  } DuckyCondResult;
  #define DUCKY_OS_COND_MAX_DEPTH 8
  typedef struct {
      DuckyOs target_os;
      uint8_t depth;                       /* open IF_OS blocks */
      bool execute[DUCKY_OS_COND_MAX_DEPTH];   /* per frame: run current branch? */
      bool matched[DUCKY_OS_COND_MAX_DEPTH];   /* IF_OS condition matched target */
      bool in_else[DUCKY_OS_COND_MAX_DEPTH];   /* currently in the ELSE_OS branch */
  } DuckyOsCondState;
  void ducky_os_cond_reset(DuckyOsCondState* st, DuckyOs initial_os);
  DuckyCondResult ducky_os_cond_eval(DuckyOsCondState* st, const char* line);
  bool ducky_os_cond_skipping(const DuckyOsCondState* st); /* true if top frame not executing */
  ```
- Consumes: nothing (pure). furi-free — host-compiles with only `-Iapplications/main/bad_usb/helpers`.

**Semantics (implement exactly):**
- `skipping` = `depth>0 && !execute[depth-1]`.
- A line whose first whitespace-delimited token is `IF_OS`/`ELSE_OS`/`END_IF`/`SET_OS` is a directive; else it's a normal line → return `EXECUTE` if not skipping, else `SKIP`.
- `SET_OS <name>`: if not skipping, set `target_os` (WINDOWS/MAC/LINUX, case-sensitive uppercase; also accept `MACOS` as MAC). Malformed/unknown name → `ERROR` (leave target unchanged). Return `DIRECTIVE` on success.
- `IF_OS <name>`: if `depth==DUCKY_OS_COND_MAX_DEPTH` → `ERROR` (don't push). Parse name (`ERROR` if malformed). Push frame: `matched = (name==target_os)`; `in_else=false`; `execute = parent_execute && matched` where `parent_execute = (depth==0)?true:execute[depth-1-...]` (i.e. the pre-push top, or true at depth 0). Increment depth. Return `DIRECTIVE`.
- `ELSE_OS`: if `depth==0` → `ERROR`. Set `in_else[top]=true`; recompute `execute[top] = parent_execute && !matched[top]`. Return `DIRECTIVE`.
- `END_IF`: if `depth==0` → `ERROR`. Pop (depth--). Return `DIRECTIVE`.
- Leading spaces before the token are tolerated. A directive keyword must match the whole first token (e.g. `IF_OSX` is NOT `IF_OS`).

- [ ] **Step 1: Write the failing test** `tests/test_ducky_os_cond.c`:
```c
#include "ducky_os_cond.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Run a script (NULL-terminated line array) with target_os; collect the
 * concatenated first-chars of EXECUTE lines into out (for easy assertions). */
static void run(const char** lines, DuckyOs os, char* out) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, os);
    int o = 0;
    for(int i = 0; lines[i]; i++) {
        DuckyCondResult r = ducky_os_cond_eval(&st, lines[i]);
        if(r == DUCKY_COND_EXECUTE) out[o++] = lines[i][0];
    }
    out[o] = 0;
}

static void test_basic_if(void) {
    const char* s[] = {"IF_OS WINDOWS","Wline","ELSE_OS","Lline","END_IF","after",0};
    char out[32];
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"Wa")==0); /* Wline + after */
    run(s, DUCKY_OS_LINUX, out);   assert(strcmp(out,"La")==0); /* Lline + after */
    printf("test_basic_if OK\n");
}

static void test_no_else(void) {
    const char* s[] = {"IF_OS MAC","Mline","END_IF","x",0};
    char out[32];
    run(s, DUCKY_OS_MAC, out);     assert(strcmp(out,"Mx")==0);
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"x")==0);
    printf("test_no_else OK\n");
}

static void test_nested(void) {
    const char* s[] = {"IF_OS WINDOWS","A","IF_OS MAC","B","ELSE_OS","C","END_IF","D","END_IF","E",0};
    char out[32];
    /* target WINDOWS: outer matches; inner MAC != WINDOWS -> C; so A,C,D,E */
    run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"ACDE")==0);
    /* target MAC: outer WINDOWS!=MAC -> whole outer skipped -> only E */
    run(s, DUCKY_OS_MAC, out);     assert(strcmp(out,"E")==0);
    printf("test_nested OK\n");
}

static void test_set_os(void) {
    const char* s[] = {"SET_OS LINUX","IF_OS LINUX","L","END_IF",0};
    char out[32]; run(s, DUCKY_OS_WINDOWS, out); assert(strcmp(out,"L")==0);
    printf("test_set_os OK\n");
}

static void test_malformed_and_unbalanced(void) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, DUCKY_OS_WINDOWS);
    assert(ducky_os_cond_eval(&st, "END_IF") == DUCKY_COND_ERROR);   /* unbalanced */
    assert(ducky_os_cond_eval(&st, "ELSE_OS") == DUCKY_COND_ERROR);  /* unbalanced */
    assert(ducky_os_cond_eval(&st, "IF_OS BSD") == DUCKY_COND_ERROR);/* bad os */
    assert(ducky_os_cond_eval(&st, "SET_OS ZZZ") == DUCKY_COND_ERROR);
    assert(ducky_os_cond_eval(&st, "IF_OSX WINDOWS") == DUCKY_COND_EXECUTE); /* not a directive */
    printf("test_malformed_and_unbalanced OK\n");
}

static void test_overnest(void) {
    DuckyOsCondState st; ducky_os_cond_reset(&st, DUCKY_OS_WINDOWS);
    for(int i = 0; i < DUCKY_OS_COND_MAX_DEPTH; i++)
        assert(ducky_os_cond_eval(&st, "IF_OS WINDOWS") == DUCKY_COND_DIRECTIVE);
    assert(ducky_os_cond_eval(&st, "IF_OS WINDOWS") == DUCKY_COND_ERROR); /* depth exceeded */
    printf("test_overnest OK\n");
}

int main(void) {
    test_basic_if(); test_no_else(); test_nested(); test_set_os();
    test_malformed_and_unbalanced(); test_overnest();
    printf("test_ducky_os_cond: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails.** Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Wall -Wextra -Iapplications/main/bad_usb/helpers tests/test_ducky_os_cond.c applications/main/bad_usb/helpers/ducky_os_cond.c -o /tmp/toc && /tmp/toc'` Expected: FAIL (no file).

- [ ] **Step 3: Write `ducky_os_cond.h`** — the interfaces above, furi-free (`<stdint.h>`/`<stdbool.h>`), doc comments.

- [ ] **Step 4: Write `ducky_os_cond.c`** — whole body `#ifndef PUBLIC_BUILD ... #endif`. `#include "ducky_os_cond.h"` + `<string.h>`. Implement the semantics above. Helper: parse the first token (skip leading spaces, `strcspn` on " "); a `parse_os(const char* arg, DuckyOs* out)` that matches WINDOWS/MAC/MACOS/LINUX. `parent_execute` at push time = `(depth==0)?true:execute[depth-1]`. Keep everything bounds-checked against `DUCKY_OS_COND_MAX_DEPTH`.

- [ ] **Step 5: Run test to verify it passes.** Expected: `test_ducky_os_cond: ALL PASS`, `-Wall -Wextra` clean.

- [ ] **Step 6: Commit**
```
git add applications/main/bad_usb/helpers/ducky_os_cond.c applications/main/bad_usb/helpers/ducky_os_cond.h tests/test_ducky_os_cond.c
git commit -m "badusb: DuckyScript OS-conditional block state machine + host test (USB1 Task 1)"
```

---

### Task 2: Interpreter integration + Target-OS selector + private build

**Files:**
- Modify: `applications/main/bad_usb/helpers/ducky_script_commands.c` (add gated `SET_OS`/`IF_OS`/`ELSE_OS`/`END_IF` handling)
- Modify: `applications/main/bad_usb/helpers/ducky_script.c` and/or `helpers/ducky_script_i.h` (hold a `DuckyOsCondState` on `BadUsbScript`; consult it in the per-line exec path)
- Modify: the BadUSB config scene (add a gated "Target OS" selector) — find it under `applications/main/bad_usb/scenes/`
- (Read first: `helpers/ducky_script.c` per-line execution loop + `ducky_execute_cmd`; `helpers/ducky_script_i.h` for `BadUsbScript`; the config scene for the VariableItem pattern.)

**Interfaces:**
- Consumes: Task 1 `ducky_os_cond_*` + `DuckyOs`/`DuckyOsCondState`.

- [ ] **Step 1: State** — add `#ifndef PUBLIC_BUILD DuckyOsCondState os_cond; #endif` to `BadUsbScript` (`ducky_script_i.h`), and `ducky_os_cond_reset(&script->os_cond, <default/selected os>)` where the script is initialized/started.
- [ ] **Step 2: Per-line hook** — in the line-execution path (before `ducky_execute_cmd` runs a line): `#ifndef PUBLIC_BUILD` call `ducky_os_cond_eval(&script->os_cond, line)`; if `DIRECTIVE` → consume the line (do nothing further); if `SKIP` → no-op the line; if `ERROR` → treat as a no-op (optionally set an error string, but do not crash/abort the script); if `EXECUTE` → fall through to normal `ducky_execute_cmd`. `#endif` — in a public build the hook compiles out and lines run as today. Make sure the hook sees RAW lines including the directive keywords (before REM/command stripping if that matters — verify against the exec loop).
- [ ] **Step 3: Config selector** — in the BadUSB config scene, add (gated) a "Target OS" VariableItem {Windows, Mac, Linux} stored on the app/script; use it as the `initial_os` in the reset. Mirror the existing config-row idiom (keyboard layout / USB name rows).
- [ ] **Step 4: Private m5stick build (fresh-log).** Delete `build_m5stick_s3`; run `build_m5stick_s3.bat` absolute-path. Verify `SENTINEL_OK` + `ducky_os_cond.c.obj` + the changed `.obj`s recompiled. Fix + rebuild until green.
- [ ] **Step 5: Commit**
```
git add <the touched files by explicit path>
git commit -m "badusb: wire OS-conditional blocks into interpreter + Target-OS selector, private (USB1 Task 2)"
```

---

### Task 3: Full verification + public exclusion + report

**Files:**
- Create: `docs/superpowers/plans/usb1-report.md`

- [ ] **Step 1: Private cardputer build (fresh-log)** — `build_adv_only_private.bat` → `SENTINEL_OK` + objs recompiled.
- [ ] **Step 2: Public exclusion build (fresh-log)** — `build_m5stick_s3_public.bat` → `SENTINEL_OK` + `NOCAR PASS`.
- [ ] **Step 3: nm/map exclusion scan** — GNU `nm` on public `furi_esp32.elf`: total symbols large (tool works), a known bad_usb public symbol present (proves bad_usb IS in public), then grep for `ducky_os_cond`/`os_cond`/`IF_OS` handler symbols → **0** in ELF **and** map. (bad_usb ships public, but the USB1 additions must not.)
- [ ] **Step 4: Host test** — re-run `test_ducky_os_cond` → ALL PASS.
- [ ] **Step 5: Report** `docs/superpowers/plans/usb1-report.md` — what shipped, build results (3 targets), 0-new-public-symbol proof (with a note that bad_usb itself IS public but USB1 is gated out), host-test results, honest limits (operator-selected reliable, no auto-detect), HW-deferred.
- [ ] **Step 6: Commit** the report.
- [ ] **Step 7: Final whole-branch review** of the USB1 commits (gating correctness incl. public compile-out, state-machine robustness, no crash on malformed scripts, honest scoping).

## Self-review notes
- **Spec coverage:** state machine (T1), interpreter+selector+private build (T2), verification incl. public exclusion + report (T3). Auto-detect intentionally excluded (operator-selected only). All mapped.
- **Private-gated:** every addition `#ifndef PUBLIC_BUILD`; T3 proves 0 new public symbols while confirming bad_usb itself stays public.
- **Type consistency:** `DuckyOs`/`DuckyOsCondState`/`ducky_os_cond_*` identical across T1/T2.
- **Robustness:** malformed/unbalanced/over-nesting → defined results, never crash; host-tested.
