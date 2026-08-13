# USB1 — BadUSB OS-Conditional Autorun — verification report

**Date:** 2026-08-13
**Branch:** `feat/redteam-gapfill` (PRIVATE, unpushed). Program item USB1 (`redteam-gapfill-roadmap.md`).

## What shipped
DuckyScript OS-conditional block control-flow in the BadUSB app: one payload branches per host OS.

| Layer | Files | Commit(s) |
|-------|-------|-----------|
| Conditional state machine (pure/host-tested) | `helpers/ducky_os_cond.{c,h}` | 43b20de |
| Interpreter hook + Target-OS selector + REPEAT guard | `helpers/ducky_script.{c,h,_i.h}`, `bad_usb_app.{c,_i.h}`, `scenes/bad_usb_scene_{config,work}.c` | 4719917, d405e5d |

- **New DuckyScript directives:** `IF_OS <WINDOWS|MAC|LINUX> … [ELSE_OS] … END_IF` and `SET_OS <os>`. A pure block/skip state machine (nesting depth ≤8) decides which lines run.
- **Target OS is operator-selected** — a "Target OS" config-scene row (Windows/Mac/Linux) sets the default; `SET_OS` overrides at runtime. **No auto-detect** (the codebase exposes no reliable USB-layer OS fingerprint — only a heuristic LED read-back deliberately not shipped).
- **Hook placement:** `ducky_parse_line` evaluates the raw trimmed line (directives intact, before command dispatch) — the single choke point both the normal and REPEAT-replay paths funnel through. DIRECTIVE/SKIP → next line; ERROR (malformed/unbalanced) → logged safe no-op, never aborts the script; EXECUTE → normal dispatch.
- **REPEAT safety:** REPEAT never replays an OS-cond directive (that would desync the block stack) — directive-ness is tracked per line and the replay no-ops for a directive `line_prev`.

## Gating (bad_usb is a PUBLIC app)
Unlike WF1/CAN1's private contexts, BadUSB ships in public builds. So **every** USB1 addition is `#ifndef PUBLIC_BUILD`: the state-machine `.c` body, the `DuckyOsCondState`/`target_os`/directive-flag fields, the hook, the resets, and the config row. Public row-index numbering is preserved via `CONFIG_INDEX_LAST_COMMON`.

## Build gate — all fresh-log verified
| Target | Script | Result |
|--------|--------|--------|
| Private m5stick S3 | `build_m5stick_s3.bat` | `SENTINEL_OK` (log 17:18:10) |
| Private Cardputer ADV | `build_adv_only_private.bat` | `SENTINEL_OK` |
| **Public** m5stick S3 | `build_m5stick_s3_public.bat` | `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS` (log 17:26:19) |

The public build compiling green is itself the proof that all guards strip cleanly (no dangling reference to a gated field/symbol).

## Public exclusion proof
GNU `nm` on public `furi_esp32.elf` (26,531 symbols):
```
bad_usb IS in public:  ducky_execute_cmd present = 1   (the app ships public)
USB1 symbols (ducky_os_cond|os_cond|DuckyOs):
  ELF match count: 0
  MAP match count: 0
```
**BadUSB is present in public, but 0 USB1 symbols are** — the OS-conditional feature is fully gated out while the app itself remains public.

## Host tests — ALL PASS (`-Wall -Wextra` clean, WSL gcc)
`test_ducky_os_cond` (6 cases): basic IF/ELSE, no-else, nested blocks, SET_OS, malformed/unbalanced directives (defined ERROR, no crash), over-nesting (depth>8 rejected).

## Review outcomes (per-task, all Approved)
- **Task 1:** nesting-skip invariant + bounds hand-traced against source — hold. 2 cosmetic minors deferred.
- **Task 2:** gating verified (public compile-out clean, no unguarded reference). Fixed 1 Important — REPEAT replaying a directive desynced the block stack (silent wrong-branch); now tracked + no-op'd. Scoped re-review confirmed the fix (flag-timing lockstep) with no new defect.

## Honest limits (surfaced in docs/UI)
- **Reliable OS targeting is operator-selected** (config selector + `SET_OS`) — there is no magic auto-detection.
- **No USB-layer OS fingerprint** (WCID/control-request timing) is reachable in this codebase; the only host→device signal is a keyboard-LED heuristic, deliberately NOT shipped (it can guess wrong).
- Nesting bounded (≤8); malformed/unbalanced directives are defined no-ops (logged), never crashes or silent wrong-branch.

## HW status — DEFERRED
Host-logic + build-gate + symbol-exclusion verified. Not exercised on real Windows/macOS/Linux hosts. On-host confirmation that each branch types correctly is the remaining proof.
