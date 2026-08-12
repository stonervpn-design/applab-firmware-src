# nRF24 Red-Team Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add Sniffer+Replay, KeySniffer, Drone/RC hijack, and a packet Fuzzer to the `nrf24` app as one modular suite on a shared capture/TX engine.

**Architecture:** New `helpers/nrf24_capture` engine (record + bounded store + replay/raw-TX) reusing `nrf24_hw` + `nrf24_mj_core`. Four feature layers (Sniffer, KeySniffer, Drone, Fuzzer) each = a pure host-testable logic helper + a firmware scene. All PRIVATE-only.

**Tech Stack:** C (ESP32-S3 / ESP-IDF v5.4.1), Flipper-port `nrf24` app; host unit tests via WSL gcc. Spec: `docs/superpowers/specs/2026-08-12-nrf24-redteam-suite-design.md`.

## Global Constraints

- **PRIVATE-only.** Every new scene/helper/UI is guarded `#ifndef PUBLIC_BUILD` (functions, ADD_SCENE lines, menu items, and any alloc-time free safety-net — the P3 gc gotcha: an unconditional `free()` anchors a whole view into public even if the scene is gated). The public build must gain ZERO new nRF24 attack symbols.
- **PRE-EXISTING ISSUE to verify (not this feature's job to fully fix, but flag):** the `nrf24` app currently has NO `PUBLIC_BUILD` guards — confirm whether it's in the public app list (`fam_config.py`) and whether MouseJack/jammer already ship in public. Report to the human; do NOT silently expand scope to gate the whole existing app unless asked. My NEW code is individually gated regardless.
- **Build via the PowerShell tool, verify FRESH logs (a stale-log false-green bug exists):** DELETE the build dir first, invoke the `.bat` by ABSOLUTE path (`& cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_x.bat"`), then confirm the log mtime is CURRENT and read its terminal `SENTINEL_OK`. NEVER trust exit codes or an un-refreshed log. Private targets: `build_m5stick_s3.bat`, `build_adv_only_private.bat`. Public EXCLUSION check only: `build_m5stick_s3_public.bat` → `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR PASS` (and no new nRF24 symbols in the public `.map`).
- Host tests: WSL gcc, pure logic only. If a helper's header drags in furi (via `nrf24_hw.h`/furi), extract the needed enum/struct to a leaf header so the helper + test compile with `-Iapplications/main/nrf24/helpers` alone (the KeeLoq `GenType` leaf-header precedent).
- Reused APIs: `nrf24_hw_{init,set_channel,write_reg,read_reg,write_buf,cmd,ce,acquire,release,probe}`; `mj_fingerprint`, `mj_setup_tx_for_target`, `MjTarget`; scenes via `ADD_SCENE(nrf24_app, <name>, <Name>)` in `scenes/scene_config.h`; fuzzer via `subghz/helpers/fuzzer_mutate`.
- Commit trailer on every commit:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01Kk6c15xdcA4LAim6BJnEtv`

**Right-sizing note:** each layer = one pure-logic task (host-tested, gated) + one scene task (firmware-built). The pure-logic tasks are where correctness is proven; the scene tasks are build-green + manual/HW-deferred.

---

### Task 1: Capture engine — `helpers/nrf24_capture.{c,h}` + host test
**Files:** create `applications/main/nrf24/helpers/nrf24_capture.{c,h}`, `tests/test_nrf24_capture.c`.
**Produces:** `typedef struct { uint8_t addr[5]; uint8_t channel; uint8_t payload[32]; uint8_t len; uint32_t ts; uint8_t proto; } Nrf24Capture;` `typedef enum { N24_RAW, N24_MOUSEJACK, N24_KEYBOARD, N24_BAYANG, N24_SYMA, N24_ESB, N24_V2X2 } Nrf24Proto;` plus a bounded store: `void nrf24_capture_reset(void); bool nrf24_capture_add(const Nrf24Capture*); size_t nrf24_capture_count(void); const Nrf24Capture* nrf24_capture_get(size_t i);` (`N24_CAP_MAX 64`, ring-drops oldest).
- [ ] Header is furi-free (only `<stdint.h>/<stdbool.h>/<stddef.h>`) so it host-compiles alone.
- [ ] Implement the fixed-capacity store (static array, wrap at MAX, count saturates at MAX).
- [ ] Host test: add N<MAX → count==N + get returns them in order; add >MAX → count==MAX + oldest dropped; reset → count 0; get out-of-range → NULL. TDD.
- [ ] Commit + report.

### Task 2: Sniffer + Replay scene — `scenes/scene_sniffer.c`
**Files:** create `scenes/scene_sniffer.c`; modify `scenes/scene_config.h` (`ADD_SCENE(nrf24_app, sniffer, Sniffer)`, PRIVATE-gated), `nrf24_capture.c` (add `nrf24_capture_replay`/`nrf24_tx_raw`).
**Consumes:** Task 1 store; the MouseJack promiscuous RX loop (read `scene_mj_scan.c` — reuse its hop+read+`mj_fingerprint` pattern).
- [ ] `void nrf24_capture_replay(const Nrf24Capture*, uint8_t repeats)` and `void nrf24_tx_raw(const uint8_t addr[5], uint8_t chan, const uint8_t* payload, uint8_t len, uint8_t repeats)` in `nrf24_capture.c` (config via `mj_setup_tx_for_target`-style + `nrf24_hw` write_buf/cmd/ce burst).
- [ ] Scene: Scan (promiscuous hop) appends validated frames to the store (live count); a list view (`mj_format_target`-style rows) → select → Replay (1 / N-burst) with abort; `#ifndef PUBLIC_BUILD` whole file.
- [ ] Build private m5stick green (fresh log). Commit + report. (SD `/ext` save deferred — open item.)

### Task 3: KeySniffer decode — `helpers/nrf24_keysniffer.{c,h}` + host test
**Files:** create `helpers/nrf24_keysniffer.{c,h}`, `tests/test_nrf24_keysniffer.c`.
**Produces:** `bool nrf24_keysniffer_decode(const Nrf24Capture* cap, char* out_ch, uint8_t* out_hid, uint8_t* out_mod);` — for the unencrypted-keyboard classes (KeySniffer set), map a captured HID-over-nRF payload → USB HID keycode + modifier → ASCII. Pure (USB HID usage table).
- [ ] Encode the vulnerable-keyboard payload layout(s) (v1: the documented KeySniffer/MoKeyboard unencrypted-HID format — the plan's open item names the exact model set; start with the generic 8-byte HID report offset the class uses) + the standard USB HID → ASCII table (with shift).
- [ ] Host test: known payload vectors → expected chars (letters, digits, shift, space, enter); a non-keyboard/garbage payload → returns false. TDD.
- [ ] Commit + report.

### Task 4: KeySniffer scene — `scenes/scene_keysniffer.c`
**Files:** create `scenes/scene_keysniffer.c`; modify `scenes/scene_config.h` (`ADD_SCENE(... keysniffer, KeySniffer)`, gated).
- [ ] Scene: promiscuous RX tuned to keyboard patterns → `nrf24_keysniffer_decode` per frame → append decoded chars to a scrolling log view; header states "unencrypted keyboards only"; abort on Back; `#ifndef PUBLIC_BUILD`.
- [ ] Build private m5stick green. Commit + report.

### Task 5: Drone frame builders — `helpers/nrf24_drone.{c,h}` + host tests (REFERENCE-GATED)
**Files:** create `helpers/nrf24_drone.{c,h}`, `tests/test_nrf24_drone.c`.
**Produces:** per-protocol pure builders + a registry:
`typedef struct { Nrf24Proto proto; const char* name; void (*build_control)(const uint8_t txaddr[5], uint8_t throttle, int8_t yaw, int8_t pitch, int8_t roll, uint8_t flags, uint8_t out[32], uint8_t* out_len); const uint8_t* (*hop_table)(uint8_t* n); } Nrf24DroneProto;` for **Bayang, Syma X5C, Bitcraze ESB, V2x2/CX-10**, plus `bind` variants where the protocol needs one.
- [ ] **Reference dependency (do this first):** these are REAL wire protocols — do NOT invent byte layouts. Port each from a known reference (Multiprotocol_Nano `Protocols/*.ino` — Bayang/Symax/ESB(NRF250K)/V2x2, or Deviation) and capture that reference's expected output as test vectors. If you cannot obtain the reference source in this environment, STOP and report NEEDS_CONTEXT (the human can supply the reference or a capture) — a fabricated protocol will not fly a drone.
- [ ] Implement Bayang first (best-documented: 15-byte payload, checksum = sum of bytes 0..13, address/hop derived from txid) with its reference vector; then Syma X5C, Bitcraze ESB, V2x2/CX-10 similarly.
- [ ] Host test: each `build_control` at fixed inputs == the reference vector byte-for-byte; checksum correctness; hop_table length/values match reference. TDD per protocol.
- [ ] Commit + report (note which protocols are vector-verified vs pending reference).

### Task 6: Drone hijack scene — `scenes/scene_drone.c`
**Files:** create `scenes/scene_drone.c`; modify `scenes/scene_config.h` (gated).
**Consumes:** Task 5 registry; `nrf24_tx_raw`; the Sniffer for bind-sniff.
- [ ] Protocol picker → bind (sniff a target address via the capture engine, or generate one) → control loop: TX `build_control` at the protocol rate over its hop table; buttons drive throttle/yaw/pitch/roll (2-button: select-axis + inc/dec via cycle+Ok; Cardputer: direct keys); prominent abort (Back cuts TX). `#ifndef PUBLIC_BUILD`.
- [ ] Build private m5stick green. Commit + report.

### Task 7: Fuzzer mutators — `helpers/nrf24_fuzz.{c,h}` + host test
**Files:** create `helpers/nrf24_fuzz.{c,h}`, `tests/test_nrf24_fuzz.c`.
**Produces:** `typedef enum { N24F_BITFLIP, N24F_LENSWEEP, N24F_FIELD, N24F_RANDOM } Nrf24FuzzMode; void nrf24_fuzz_next(const uint8_t* base, uint8_t base_len, Nrf24FuzzMode mode, uint32_t seed, uint32_t step, uint8_t out[32], uint8_t* out_len);` — deterministic given (base, mode, seed, step); mirror `subghz/helpers/fuzzer_mutate` shape.
- [ ] Implement the 4 mutators (bit-flip walks the step-th bit; len-sweep varies out_len; field cycles a byte; random is seeded-PRNG — pass seed/step in, no `Math.random`).
- [ ] Host test: determinism (same inputs → same output); bit-flip flips exactly one bit at `step`; len-sweep bounds 1..32; random reproduces for a fixed seed. TDD.
- [ ] Commit + report.

### Task 8: Fuzzer scene — `scenes/scene_fuzz.c`
**Files:** create `scenes/scene_fuzz.c`; modify `scenes/scene_config.h` (gated).
- [ ] Scene: pick base (a captured entry or a chosen protocol's valid frame) + target addr/chan + mode → TX mutated frames via `nrf24_tx_raw` in a rate/abort loop; on-air indicator; header states "effect is target-dependent — reports what it sent, not a crash verdict"; `#ifndef PUBLIC_BUILD`.
- [ ] Build private m5stick green. Commit + report.

### Task 9: RF24 Suite menu + registration + gating audit
**Files:** modify `scenes/scene_menu.c` (add a "RF24 Suite" submenu or the 4 new entries, PRIVATE-gated), `scenes/scene_config.h` (all 4 ADD_SCENEs gated), and confirm every new file's whole body is `#ifndef PUBLIC_BUILD`.
- [ ] Wire the 4 scenes into the menu (gated). Verify no alloc-time unconditional `free()`/view-anchor pulls a gated view into public.
- [ ] Build private m5stick + cardputer green (fresh logs). Commit + report.

### Task 10: Full verification + public exclusion + report
**Files:** create `docs/superpowers/plans/nrf24-suite-report.md`.
- [ ] Build all three targets with the fresh-log method: private m5stick + private cardputer → `SENTINEL_OK`; public m5stick → `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR PASS`.
- [ ] `nm`/map-scan the PUBLIC `.map`/elf for the new symbols (`nrf24_capture_*`, `nrf24_keysniffer_*`, `nrf24_drone_*`, `nrf24_fuzz_*`, `scene_sniffer/keysniffer/drone/fuzz`) → must be **0** (gc'd out of public).
- [ ] Run the 4 host tests (capture, keysniffer, drone, fuzz) → all PASS.
- [ ] Report: what shipped, host-test results, which drone protocols are vector-verified, the pre-existing nrf24-app-public gating finding, and the honest limits + HW-deferred status.
- [ ] Commit + report. Final whole-branch review.

## Self-review notes
- **Spec coverage:** engine (T1/T2), Sniffer+Replay (T2), KeySniffer (T3/T4), Drone (T5/T6), Fuzzer (T7/T8), menu+gating (T9), verification incl. public-exclusion (T10). All mapped.
- **Reference honesty:** T5 explicitly forbids fabricating drone protocols — port-from-reference + vector-gate, or NEEDS_CONTEXT. This is the highest-risk task.
- **Private-only:** every scene/helper gated; T9 audits the gc-anchor gotcha; T10 proves 0 new public symbols.
- **Open items (resolve in-task, non-blocking):** exact `nrf24_hw` promiscuous-RX primitive (T2, from mj_scan); the KeySniffer keyboard model set (T3); drone reference vectors (T5); `/ext` SD save (deferred); the pre-existing nrf24-app public-gating question (T1 constraint + T10 report).
