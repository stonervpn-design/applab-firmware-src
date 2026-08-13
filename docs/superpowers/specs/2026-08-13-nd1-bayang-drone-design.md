# ND1 — Bayang Drone Control (nRF24) — design spec

**Date:** 2026-08-13
**Status:** Proposed — awaiting user approval
**Scope:** PRIVATE build only. `applications/main/nrf24/`. ONE protocol (Bayang) as a
complete vertical; extend to Syma/ESB/V2x2 only after HW confirms Bayang flies.
**Program:** gap-fill item ND1 (see `docs/superpowers/redteam-gapfill-roadmap.md`).

## Goal
A "Drone (Bayang)" screen in the nRF24 app that binds to and controls a Bayang-protocol
toy quadcopter (a very common ~$20 nRF24 quad, and the protocol family behind many
"Eachine/JJRC/Floureon" toys). Bind → then a control loop transmitting throttle / yaw /
pitch / roll driven by the device buttons. This is the deferred nRF24-suite drone layer,
now built from PUBLIC documented formats.

## HONEST STATUS — READ FIRST (this is the one feature without a real proof point)
- The Bayang packet layout, checksum, bind sequence, and channel-hop table are
  **transcribed from the public GPL Deviation / Multiprotocol TX projects** (well-known
  open sources). I have **no reference capture and no live drone** to verify against.
- Therefore **every wire-format constant is UNVERIFIED against real hardware.** A subtle
  bit-order / checksum / timing / channel error would compile clean, pass the structural
  host tests, and still not fly anything. The on-screen UI and all docs say so plainly.
- Host tests prove **internal consistency only** (checksum determinism, packet length,
  field packing round-trips, channel-hop stays in band) — NOT that the bytes match what a
  real Bayang receiver expects.
- **The real proof is on-hardware: you bind to an actual Bayang quad.** If it flies, I
  extend to the other 3 families; if not, we debug against a capture. No fabricated
  "it works" claim anywhere.

## Feasibility note — PHY config (real gap to close)
The existing `nrf24_hw_tx_setup()` hardcodes **2 Mbps data rate + CRC off** (the
MouseJack/ESB config). **Bayang requires 1 Mbps + 16-bit CRC on.** So ND1 must set the
radio PHY correctly — either:
- **(A)** add `nrf24_hw_tx_setup_ex(addr, addr_len, channel, payload_len, data_rate, crc)`
  (a superset; the existing `nrf24_hw_tx_setup` becomes a thin wrapper) — cleanest, and
  reusable by the future Syma/V2x2 protocols which have their own rates; **or**
- **(B)** the drone code pokes `RF_SETUP` (data rate) + `CONFIG` (EN_CRC/CRCO) via
  `nrf24_hw_write_reg` directly after setup.
Plan will use **(A)** (the honest, reusable fix). This is gated too.

## Constraints (hard)
- **PRIVATE-only**, `#ifndef PUBLIC_BUILD` (matches the nRF24 suite's gating — the
  Sniffer/KeySniffer/Fuzz block in `scene_config.h`; public nm-scan = 0 new symbols).
- **No new radio driver** — reuse `nrf24_hw_*` (+ the gated `tx_setup_ex` addition).
- **Honest UI + docs** — UNVERIFIED labeling (above); no crash/fly verdict faked.
- **Bounded output** — packet buffer exactly the protocol length (16 for Bayang);
  builders never write past it; all fields clamped to valid ranges.
- Fresh-log builds; host tests via WSL gcc for the pure builder.

## Architecture
```
  helpers/drone_bayang.{c,h}  (NEW, pure, host-tested, furi-free)
     bayang_build_bind(txid[3], out[16])
     bayang_build_control(txid[3], throttle,yaw,pitch,roll,flags, out[16])
     bayang_checksum(...)  + the RF channel-hop table
        │  16-byte packet
        ▼
  nrf24_hw (tx_setup_ex: 1Mbps + CRC-on, set_channel, tx_send)   [reuse + gated ext]
        │
        ▼
  scenes/scene_drone_bayang.c  (NEW, gated) — bind phase -> control loop
     worker: hop channel per packet + tx_send at Bayang's rate; buttons drive axes;
     Back aborts (stop-flag + join). On-air + UNVERIFIED banner.
```

## Components

### 1. Bayang builder — `helpers/drone_bayang.{c,h}` (NEW, pure/furi-free, host-tested)
- `#define BAYANG_PKT_LEN 16` (15 payload + convention per the source), `#define BAYANG_RF_NUM_CH 4`.
- `void bayang_build_bind(const uint8_t txid[3], uint8_t out[BAYANG_PKT_LEN]);`
- `void bayang_build_control(const uint8_t txid[3], uint16_t throttle, uint16_t yaw, uint16_t pitch, uint16_t roll, uint8_t flags, uint8_t out[BAYANG_PKT_LEN]);`
  - Packs the Bayang byte layout (flags byte, big-endian 10/12-bit channel fields for
    throttle/yaw/pitch/roll with their documented offsets, the 3-byte txid, and the
    trailing checksum). Exact field positions/encodings transcribed from the public
    Deviation `bayang.c`. Channels clamped to their valid ranges.
- `uint8_t bayang_checksum(const uint8_t* pkt, uint8_t len);` — the documented Bayang
  sum/xor checksum over the payload (transcribed; host-tested for determinism).
- `void bayang_rf_channels(const uint8_t txid[3], uint8_t out_ch[BAYANG_RF_NUM_CH]);` —
  derive the 4-channel hop set from the txid per the documented rule; every channel
  validated ≤ 125 (nRF24 max).
- A fixed/derived bind txid (or a simple generator) — bind uses the documented bind
  address/channel.

### 2. Control scene — `scenes/scene_drone_bayang.c` (NEW, gated)
- **Bind phase:** TX bind packets on the bind channel(s) for ~N seconds (a worker), then
  transition to control. A "binding…" indicator.
- **Control phase:** worker loop hops the 4 RF channels round-robin and `tx_send`s a
  control packet each ~ (Bayang frame period) ms; the buttons drive a selected axis
  (2-button M5Stick: cycle selected axis + inc/dec via the app's input scheme; Cardputer:
  direct keys). Throttle/yaw/pitch/roll shown on screen.
- Header banner (verbatim): "UNVERIFIED wire format (from public docs) -- may not control
  a real drone. HW-test required." Back aborts (stop-flag + join, same shape as the
  nRF24 Fuzz scene). On-air indicator.
- Register `ADD_SCENE(nrf24_app, drone_bayang, DroneBayang)` + a "Drone (Bayang)" submenu
  item + dispatch in the nRF24 menu — all `#ifndef PUBLIC_BUILD`.

### 3. `nrf24_hw_tx_setup_ex` (gated additive) — `nrf24_hw.{c,h}`
- `void nrf24_hw_tx_setup_ex(const uint8_t* addr, uint8_t addr_len, uint8_t channel, uint8_t payload_len, uint8_t data_rate /*0=1Mbps,1=2Mbps,2=250k*/, bool crc16);`
  Sets `RF_SETUP` data-rate bits + `CONFIG` EN_CRC/CRCO accordingly; otherwise identical
  to `nrf24_hw_tx_setup`. The existing `nrf24_hw_tx_setup` becomes
  `tx_setup_ex(..., 1 /*2Mbps*/, false)`. **This one is `#ifndef PUBLIC_BUILD`? No** — it
  is a benign HAL superset with no attack semantics; but to keep the public surface
  identical, gate the NEW function `#ifndef PUBLIC_BUILD` and leave the existing
  `nrf24_hw_tx_setup` wrapper only in private (or keep both public but unused in public).
  Plan will gate the ex-variant + the wrapper delegation under private to guarantee 0 new
  public symbols; the public build keeps today's `nrf24_hw_tx_setup` body verbatim.

## Data flow
txid → build_bind → tx (bind chan) → build_control(axes) → hop 4 ch + tx at rate → repeat.

## Testing
- **Host (WSL gcc, pure):** `tests/test_drone_bayang.c` — checksum determinism (same input
  → same byte) and that it actually covers the payload; `bayang_build_control` packs a
  known (throttle,yaw,pitch,roll) into the expected byte offsets/values per the
  transcribed layout and the checksum validates; channel-derivation stays ≤125; field
  clamping (out-of-range throttle clamps, no overflow); bind vs control differ in the
  documented flag byte. **These prove internal consistency, NOT real-frame correctness.**
- **Build gate:** private m5stick + cardputer green (fresh logs); public m5stick
  `SENTINEL_OK` + `NOCAR PASS`; nm/map scan for `drone_bayang`/`bayang_`/`tx_setup_ex`
  symbols → 0 in public.
- **HW (the real proof, deferred to YOU):** bind to a real Bayang quad; if it arms/flies,
  the format is right and I extend to Syma/ESB/V2x2. If not, capture + debug.

## Honest limits (surfaced in-UI / docs)
- **UNVERIFIED wire format** — transcribed from public docs, not tested against a real
  drone or capture. May not control anything. (Stated on-screen.)
- **One protocol (Bayang) only** in this step — deliberately, to de-risk before building 3
  more blind.
- **nRF24-on-ESP32 TX timing** is finicky (per the jammer overhaul) — the tight control
  loop may need on-device dwell/rate tuning.
- **Toy-grade** — this controls hobby toy quads on the Bayang protocol; not "any drone."

## Non-goals (YAGNI)
- No Syma/ESB/V2x2 yet (gated on Bayang HW success).
- No telemetry/RX-back in v1. No public exposure. No fabricated "bound/flying" verdict.

## Success criteria
- "Drone (Bayang)" reachable from the nRF24 menu (private build): bind phase + a control
  loop that TXes well-formed Bayang packets with buttons driving the axes, working abort,
  and the UNVERIFIED banner.
- The pure builder passes host consistency tests (checksum, packing, channel bounds, clamp).
- Private m5stick + cardputer green; public excludes ND1 (0 new symbols) — fresh-verified.
- Honest UNVERIFIED status surfaced in-UI; on-hardware bind test is the documented next step.
