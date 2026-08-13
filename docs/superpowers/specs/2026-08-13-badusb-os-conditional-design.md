# BadUSB OS-Conditional Autorun (USB1) — design spec

**Date:** 2026-08-13
**Status:** Proposed — awaiting user approval + one gating decision
**Tree:** `src_m5stick_s3`; work in `applications/main/bad_usb/`.
**Program:** gap-fill item USB1 (see `docs/superpowers/redteam-gapfill-roadmap.md`).

## Goal
Let a single DuckyScript payload **branch on the target host OS** — run the
Windows keystrokes on Windows, macOS keystrokes on macOS, Linux on Linux — instead
of maintaining three separate scripts. Implemented as a DuckyScript control-flow
extension: `IF_OS <WINDOWS|MAC|LINUX> … [ELSE_OS] … END_IF`, with the target OS
chosen by the operator (reliable), plus an optional best-effort auto-detect.

## Feasibility (honest, from the code map)
- `helpers/ducky_script_commands.c` has a clean `ducky_commands[]` table +
  `ducky_execute_cmd` dispatch, but **no existing conditional/branch/label/variable
  infrastructure** (only REPEAT/HOLD/RELEASE/WAIT_FOR_BUTTON_PRESS). So block
  control-flow is new work, built on the existing per-line execution loop.
- `helpers/bad_usb_hid.h` exposes `get_led_state(void* inst)` — host→device keyboard
  LED read-back. This enables a **heuristic** OS hint (e.g. NumLock default differs by
  OS/config), but it is NOT reliable OS fingerprinting. No USB-layer WCID / control-
  request OS signal is reachable in this codebase's HID layer.
- **Therefore v1's reliable path is OPERATOR-SELECTED target OS**; auto-detect via LED
  is offered only as an explicitly-labeled, overridable heuristic (or deferred).

## GATING DECISION (needs your call — changes the build)
Unlike WF1 (wlan_app) and CAN1 (can_commander, already excluded), **`bad_usb` ships in
PUBLIC builds** (it's in `fam_config.py`'s `APPS`, not excluded, and has no
`#ifndef PUBLIC_BUILD`). Two options for the new `IF_OS`/`ELSE_OS`/`END_IF`/`SET_OS`
commands:
- **(A) Private-gated (default, honors your "private-only" rule):** wrap the new
  command-table entries + handlers + skip-state in `#ifndef PUBLIC_BUILD`. Public
  builds don't recognize `IF_OS` (scripts using it degrade). Proven via nm-scan = 0
  new USB1 symbols in public.
- **(B) Public:** ship the conditionals in the public BadUSB too. Rationale: an
  `IF_OS` block is a general scripting-language feature, not car/RF attack material;
  it makes public BadUSB strictly better. Your call — the standing rule is private, so
  I default to (A) unless you pick (B).

## Architecture
```
  ducky_script parser (existing per-line loop)
        │  each line
        ▼
  helpers/ducky_os_cond.{c,h}  (NEW, pure, host-tested)
     - parse an IF_OS/ELSE_OS/END_IF/SET_OS line
     - a small block/skip state machine: given (current line, target OS,
       nesting stack) -> EXECUTE or SKIP this line
        │  execute?/skip?
        ▼
  ducky_execute_cmd (extended): consult the skip-state before running a line;
     SET_OS / IF_OS / ELSE_OS / END_IF update the state; a skipped line is a no-op.
  Config scene: a "Target OS" selector (Windows/Mac/Linux[/Auto]) stored on the script.
```

## Components

### 1. Conditional state machine — `helpers/ducky_os_cond.{c,h}` (NEW, pure, host-tested)
- `typedef enum { DUCKY_OS_WINDOWS, DUCKY_OS_MAC, DUCKY_OS_LINUX } DuckyOs;`
- A small state object: current `target_os`, a nesting stack of block states
  (`{active, matched, in_else}`) supporting reasonable nesting (depth ≤ 8), and a
  `skip` boolean derived from the stack.
- `DuckyCondResult ducky_os_cond_eval(DuckyOsCondState* st, const char* line);`
  returns one of {LINE_IS_DIRECTIVE_HANDLED (IF_OS/ELSE_OS/END_IF/SET_OS consumed),
  LINE_EXECUTE (run it), LINE_SKIP (inside a non-matching block)}. Parses the
  directive keywords + their OS argument; malformed directive → a defined error
  result (not a crash). Unbalanced END_IF / stack overflow handled gracefully.
- Pure: furi-free, so the block/skip logic is host-unit-tested exhaustively (nested
  blocks, ELSE_OS, mismatched OS, malformed, over-nesting).

### 2. Interpreter integration — extend `ducky_script_commands.c` / `ducky_script.c`
- Add `SET_OS`, `IF_OS`, `ELSE_OS`, `END_IF` to `ducky_commands[]` (or intercept them
  before the normal dispatch). Hold a `DuckyOsCondState` on `BadUsbScript`.
- In the per-line execution path: call `ducky_os_cond_eval`; if SKIP, no-op the line;
  if directive, update state; else execute normally.
- `SET_OS <WINDOWS|MAC|LINUX>` sets the target at runtime; the config-scene selector
  sets the default before run. (Option A: all of this `#ifndef PUBLIC_BUILD`.)

### 3. (Optional) `OS_DETECT` heuristic — deferred/flagged
- If you want it: an `OS_DETECT` command reads `get_led_state()` post-enumeration as a
  best-effort hint and sets `target_os`, with the UI/docs stating plainly it is an
  unreliable heuristic and the operator should prefer `SET_OS` / the selector. If you
  don't want the risk of a misleading auto-detect, we ship operator-selected only.

### 4. Config scene — a "Target OS" row
- Add a selector (Windows/Mac/Linux) to the BadUSB config scene, stored on the script,
  used as the initial `target_os`. (Gated with the rest under Option A.)

## Data flow
config "Target OS" (or SET_OS) → DuckyOsCondState.target_os → per line:
ducky_os_cond_eval → {handle directive | execute | skip} → existing key output.

## Testing
- **Host (WSL gcc, pure):** `tests/test_ducky_os_cond.c` — a script as a line array +
  a target OS → the exact set of executed lines. Cases: matching/non-matching IF_OS,
  ELSE_OS taken, nested IF_OS, SET_OS mid-script, malformed directive (defined error,
  no crash), unbalanced END_IF, over-nesting (depth>8 handled).
- **Build gate:** private m5stick + cardputer green (fresh logs). Under Option A:
  public m5stick `SENTINEL_OK` + `NOCAR PASS` + nm-scan for `ducky_os_cond`/`IF_OS`
  handler symbols → **0**. Under Option B: public builds and the feature is present
  (no exclusion scan needed; still confirm NOCAR PASS).
- **HW (deferred):** real Windows/macOS/Linux hosts to confirm the branched payloads
  type correctly; the LED heuristic (if built) validated per-host.

## Honest limits (surfaced in-UI / docs)
- **Reliable OS targeting is operator-selected**, not magic auto-detection. Any
  `OS_DETECT` is a labeled heuristic (LED-based), not a guarantee.
- **No USB-layer OS fingerprint** (WCID/control-request timing) — not exposed here.
- Nesting is bounded (depth ≤ 8); malformed directives are defined no-ops/errors, never
  crashes or silent wrong-branch execution.

## Non-goals (YAGNI)
- No general expression/variable language — only OS conditionals (+ SET_OS).
- No unreliable "auto-detect" presented as authoritative.
- No new HID/USB descriptor work.

## Success criteria
- A DuckyScript with `IF_OS WINDOWS … ELSE_OS … END_IF` runs only the matching branch,
  target set by the operator (selector or SET_OS).
- Pure conditional state machine passes exhaustive host tests (nesting/else/malformed).
- Builds green per the chosen gating option; under (A), 0 new public symbols.
- Honest limits (operator-selected reliable, auto-detect heuristic) surfaced.
