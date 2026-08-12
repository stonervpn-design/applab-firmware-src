# nRF24 Red-Team Suite — design spec

**Date:** 2026-08-12
**Status:** Approved design, pending implementation plan
**Scope:** PRIVATE build only (attack tooling). Public build must exclude it entirely.
**Tree:** `src_m5stick_s3`, branch `feat/nrf24-redteam-suite` (off the KeeLoq HEAD).
**Targets:** M5Stick S3 (COM10) + Cardputer ADV (COM12), board-agnostic via `nrf24_hw`.

## Goal

Add four nRF24 / 2.4GHz red-team capabilities to the existing `nrf24` app —
**Sniffer+Replay, KeySniffer, Drone/RC hijack, packet Fuzzer** — as one suite,
built on a shared capture/TX engine that mostly reuses what MouseJack already
provides. One spec, but the architecture stays modular so each layer can be
implemented and HW-tested independently.

## Constraints (hard)

- **PRIVATE-only.** All new scenes/UI/logic guarded `#ifndef PUBLIC_BUILD`
  (matches the standing "all attack tools private" rule; the existing
  jammer/MouseJack gating is the precedent to follow). The public build must
  compile to nothing and pass its exclusion gate — verify with a genuinely-clean
  public build (fresh log, not stale — see the build gotcha below).
- **Board-agnostic.** Ride the existing `nrf24_hw` SPI/pin abstraction (works on
  both boards today for jammer/MouseJack). No new radio driver.
- **Build via PowerShell, verify FRESH logs.** Delete the build dir, invoke the
  `.bat` by ABSOLUTE path, confirm the log mtime is current + read its terminal
  SENTINEL. NEVER trust exit codes or stale logs (a stale-log false-green bug is
  known in this tree).
- Host tests via WSL gcc (pure logic only — see Testing).

## Background — what already exists (reuse, don't rebuild)

`applications/main/nrf24/` has: `nrf24_hw.{c,h}` (radio primitives), the
`nrf24` app (menu: Spectrum / Jammer / MouseJacker), and
`helpers/nrf24_mj_core.{c,h}` which already provides:
- `int mj_fingerprint(...)` — decode a 32-byte raw **promiscuous** frame,
  validate CRC16-CCITT, fingerprint address + vendor. (The capture engine.)
- `MjTarget {address[5], channel, vendor}` — the captured-target record.
- `void mj_setup_tx_for_target(const MjTarget*)` — configure the radio for TX
  against a captured address/channel. (The TX engine.)
- `mj_inject_keystroke/string`, `mj_logitech_wake`, `mj_format_target`,
  `mj_vendor_label`.

So promiscuous RX + address discovery + targeted TX are ~70% built. The MouseJack
`scene_mj_scan.c` already runs the promiscuous RX loop; the new Sniffer reuses
that path plus `mj_fingerprint`.

Fuzzer reuse: `applications/main/subghz/helpers/fuzzer_frame.{c,h}` and
`applications/main/ble_spam/ble_walk_fuzz.{c,h}` are the existing mutation
patterns to mirror.

## Architecture

One shared engine + four feature layers, reached from a new **"RF24 Suite"**
submenu (or new entries) under the `nrf24` app:

```
        nrf24_hw (SPI/pins, both boards)  +  nrf24_mj_core (promisc decode, TX-config)
                                   │
                 ┌─────────────────┴───── nrf24_capture (NEW: generic engine) ─────────────────┐
                 │  capture record {addr[5], channel, payload[32], len, ts, proto}             │
                 │  in-memory capture list (+ optional /ext save on Cardputer SD)              │
                 │  raw replay TX; targeted TX loop                                            │
                 └───┬───────────────┬──────────────────┬───────────────────┬─────────────────┘
             Sniffer+Replay      KeySniffer         Drone/RC hijack        Fuzzer
             (capture → list      (keyboard-proto    (per-proto bind-sniff  (mutate a base
              → replay)            decode → keys)      + control TX loop)     frame → TX burst)
```

## Components

### 0. Capture/TX engine — `helpers/nrf24_capture.{c,h}` (new)
- **Capture record:** `typedef struct { uint8_t addr[5]; uint8_t channel; uint8_t payload[32]; uint8_t len; uint32_t ts; uint8_t proto; } Nrf24Capture;` (`proto` = a small enum: raw/mousejack/keyboard/bayang/syma/esb/v2x2).
- **Capture store:** a bounded in-memory ring/list (e.g. 64 entries) with add/get/clear/count. Optional persist to `/ext/nrf24/captures/*.n24` on boards with SD (Cardputer); M5Stick is in-memory only (no SD).
- **RX helper:** thin wrapper over the MouseJack promiscuous RX loop (channel hop + read 32-byte frame + `mj_fingerprint`) that appends validated frames to the store; a "raw" mode that stores any frame with a valid address, not only MouseJack-fingerprinted ones.
- **TX helpers:** `nrf24_capture_replay(const Nrf24Capture*)` (config via `mj_setup_tx_for_target`-style + burst the payload) and a generic `nrf24_tx_raw(addr, channel, payload, len, repeats)`.

### 1. Sniffer + Replay — `scenes/scene_sniffer.c` (new)
Promiscuous scan (channel sweep) → live capture list (address / channel / len,
using `mj_format_target`) → select an entry → **Replay** (single or N-burst) or
**Save** (Cardputer). Standalone value: capture & replay any nRF24 remote / toy /
RC bind frame.

### 2. KeySniffer — `helpers/nrf24_keysniffer.{c,h}` + `scenes/scene_keysniffer.c` (new)
- Pure decode layer: given captured frames on the vulnerable wireless-keyboard
  protocols (the KeySniffer / MoKeyboard unencrypted-HID classes — a fixed
  device/protocol table), map payload → HID keycode → ASCII, reconstruct the
  keystroke stream to a scrolling log.
- Sniff mode tunes the promiscuous RX to the keyboard channel/address patterns,
  then feeds `nrf24_keysniffer_decode()` per frame.
- **Only unencrypted keyboards** are decodable (stated in the UI).

### 3. Drone/RC hijack — `helpers/nrf24_drone.{c,h}` + `scenes/scene_drone.c` (new)
- **Per-protocol packet builders** (pure functions, host-tested) for **Bayang,
  Syma X5C, Bitcraze ESB, V2x2/CX-10**: `build_<proto>_bind(...)` and
  `build_<proto>_control(txaddr, throttle, yaw, pitch, roll, flags, out[32])`
  incl. each protocol's checksum/CRC and channel-hop table.
- **Bind-sniff:** use the Sniffer to grab a target's tx address/channel (or a
  manual/generated address for a fresh bind), then a **control loop** that TXes
  control frames at the protocol's rate while the buttons drive
  throttle/yaw/pitch/roll (2-button: cycle a selected axis + inc/dec; Cardputer:
  direct keys).
- Protocol registry so adding a 5th family later is one table entry.

### 4. nRF24 fuzzer — `helpers/nrf24_fuzz.{c,h}` + `scenes/scene_fuzz.c` (new)
- Mutation layer (mirror `fuzzer_frame`/`ble_walk_fuzz`): take a base frame
  (a captured entry or a chosen protocol's valid frame) and emit mutated
  variants (bit-flips, length changes, field sweeps, random) TX'd at a target
  addr/channel, with a rate/abort control. Reuses `nrf24_tx_raw`.
- Purpose: stress-test 2.4GHz receivers/toys/drones for crashes.

## Data flow

- **Capture:** RX promiscuous (hop + read) → `mj_fingerprint`/raw-validate →
  `nrf24_capture` store.
- **Replay:** pick capture → `nrf24_capture_replay` (config + burst).
- **KeySniffer:** RX (keyboard patterns) → `nrf24_keysniffer_decode` → keystroke log.
- **Drone:** bind-sniff (capture) → control loop `build_<proto>_control` → `nrf24_tx_raw` at proto rate.
- **Fuzzer:** base frame → `nrf24_fuzz` mutate → `nrf24_tx_raw` burst.

Shared contract: everything reads/writes `Nrf24Capture` + goes through
`nrf24_hw`/`mj_core`; no duplicate radio code.

## UI

New "RF24 Suite" submenu under the `nrf24` app: Sniffer / KeySniffer / Drone /
Fuzzer. 2-button-friendly on the M5Stick (cycle-to-select + Ok, per
`BOARD_TWO_BUTTON_INPUT`; tilt is wake-only, no Left/Right); full keys on the
Cardputer. Drone control screen maps buttons to throttle/yaw/pitch/roll. Each
attack screen has a clear abort (Back) and an on-air indicator.

## Testing

- **Host-tested PURE logic (the provable core, WSL gcc — like the KeeLoq registry):**
  the per-protocol drone frame builders + checksums (Bayang/Syma/ESB/V2x2)
  against known-good reference vectors (from the Multiprotocol/Deviation
  projects); the KeySniffer HID→ASCII decode tables; the fuzzer mutators
  (deterministic given a seed); `nrf24_capture` store add/get/clear/bounds.
  These are decoupled from `nrf24_hw`/furi so they compile with
  `-Iapplications/main/nrf24/helpers` and nothing else (extract any needed enum
  to a leaf header if it drags in furi — the KeeLoq `GenType` precedent).
- **Build gate:** private m5stick + cardputer green (fresh logs); public
  m5stick **exclusion** check — the whole suite `#ifndef PUBLIC_BUILD`, so the
  public build compiles to nothing and its gate passes with no new symbols.
- **HW is the real proof (deferred, needs targets):** on-device nRF24 + an actual
  vulnerable keyboard / one of the four drone families. Expect promiscuous-RX
  timing + per-channel dwell tuning (per the jammer overhaul: software-CS
  settle, dwell ≥ PLL settle).

## Honest limits (no overselling)

- **Promiscuous sniff is lossy:** the nRF24L01+ can't truly go promiscuous; the
  address-trick catches a fraction of packets and only while the target is
  transmitting. Strong for MouseJack-class devices, weak for arbitrary/encrypted.
- **KeySniffer only decodes UNENCRYPTED keyboards** (the KeySniffer CVE set); AES
  keyboards are immune — the UI says so.
- **Drone support = these 4 families**, not "all drones." Each is a real
  protocol impl; a 5th is a registry entry later.
- **Fuzzer crashes are target-dependent** — TX'ing malformed frames is easy;
  proving a crash needs the actual target. The tool reports what it sent, not a
  fabricated "vulnerable" verdict.
- **nRF24-on-ESP32 SPI timing is finicky** — fast-hop sniffing + tight TX loops
  need on-device tuning (documented, not hidden).

## Non-goals (YAGNI)

- No new nRF24 radio driver (reuse `nrf24_hw`).
- No public-build exposure (attack tooling — private only).
- No claim of universal drone/keyboard coverage.
- No on-device "did it crash?" oracle for the fuzzer.

## Success criteria

- All four features reachable + operable from the RF24 Suite menu on both boards
  (2-button + keyboard), each with a working abort.
- The pure logic (drone builders, keysniffer decode, fuzz mutators, capture store)
  passes host unit tests against reference vectors.
- Private m5stick + cardputer build green; public build compiles to nothing +
  exclusion gate passes (no new attack symbols in public), all fresh-verified.
- Honest limits surfaced in-UI where they affect the operator (lossy sniff,
  encrypted-keyboard immunity, per-protocol drones).

## Open items for the plan

- Exact `nrf24_hw` RX primitive to reuse for the raw promiscuous loop (grep the
  MouseJack scan path).
- Capture-store size + `/ext` file format + whether v1 includes SD save or defers it.
- Which specific KeySniffer keyboard models/protocols ship in v1's decode table.
- Reference test vectors for each drone protocol (source: Multiprotocol/Deviation).
- Confirm the existing `nrf24` app's public gating so new entries match it.
