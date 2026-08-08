# BLE PKE Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:executing-plans (or subagent-driven-development) to implement this task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A two-device, GATT/app-layer BLE proximity relay for passive-keyless-entry systems, on the flashed M5Stick S3 + Cardputer ADV, linked by ESP-NOW. PRIVATE-only, authorized-target testing only.

**Architecture:** Key-side device = BLE peripheral impersonating the target (phone connects to it); Car-side device = BLE central connected to the real target; app-layer GATT ops are forwarded opaquely between the two over ESP-NOW. The relay never decrypts — it blind-forwards app-layer challenge/response bytes.

**Tech Stack:** ESP-IDF 5.4.1, NimBLE (already in port: `ble_spam`, Tesla scanner), ESP-NOW (`applications/main/esp_now`), the port's FAP/app framework. No host gcc → pure logic verified by Python replica + `xtensa-esp32s3-elf-gcc -fsyntax-only`; symbols checked with `nm`; behavior gated on-device.

## Global Constraints

- **PRIVATE-only.** New app + all BLE/ESP-NOW relay code stays off public builds and the webflasher. Guard the app out of the public app list (`fam_config.py`) the same way `proto_pirate`/jammer are; `nm`-verify 0 relay symbols in every public ELF before any deploy. See [[fzcard-private-only-guardrail]].
- **Build via PowerShell**, never `cmd.exe /c` through the Bash tool (phantom no-op). Check SENTINEL_OK + log mtime + `.obj` newer than source + `nm` every build.
- **Mirror to `src_cardputer_adv`** for the Cardputer end (separate tree). Do NOT cross-flash bins (flash by MAC via `C:\FZCARD\PRIVATE\guarded_flash.py`).
- **Coexistence is the dominant risk.** BLE + ESP-NOW share one 2.4 GHz radio → time-sliced latency/jitter. Phase 0 measures it; every later phase reports the live number in-UI.
- **Honest scope banner in-UI:** works only for app-layer-crypto, loosely-timed PKE; link-encrypted + latency-hardened targets are out (hardware ceiling, not a bug).

---

## Phase 0 — Coexistence + latency harness (DO FIRST; gates the whole project)

### Task 0.1: ESP-NOW echo round-trip timer (one device, no BLE yet)

**Files:**
- Create: `applications/main/ble_relay/ble_relay_app.c` (minimal app shell, private)
- Create: `applications/main/ble_relay/espnow_link.{h,c}` (ESP-NOW init + framed send/recv)
- Create: `tools/ble_relay/frame_replica.py` (pure frame pack/unpack round-trip validator)
- Modify: `fam_config.py` (add `ble_relay` to the PRIVATE app list only)

**Interfaces:**
- Produces: `espnow_link_init(peer_mac)`, `espnow_link_send(const uint8_t* buf, size_t len)`, `espnow_link_set_rx_cb(cb)`; frame header `{seq:u16, op:u8, conn:u8, handle:u16, len:u16}` (10 B) + payload, ≤250 B ESP-NOW MTU.

- [ ] **Step 1:** Write `frame_replica.py` — pack(header,payload)→bytes, unpack→fields; assert round-trip for empty / 1B / 240B / boundary payloads, and that >240B is chunked.
- [ ] **Step 2:** Run `python tools/ble_relay/frame_replica.py`; expect `ALL PASS`.
- [ ] **Step 3:** Implement `espnow_link.c` framing to match the replica byte-for-byte; `xtensa-esp32s3-elf-gcc -fsyntax-only` on it with the IDF include set.
- [ ] **Step 4:** App shell: on OK, send an ESP-NOW frame to a second unit that echoes it; measure `furi_get_tick()` round-trip; draw min/avg/max ms on screen.
- [ ] **Step 5:** Build private (PowerShell), `nm` the app symbols present; build public, `nm` 0 relay symbols + car-free gate PASS. Commit.

### Task 0.2: BLE + ESP-NOW concurrent latency (THE gate)

**Files:**
- Modify: `applications/main/ble_relay/ble_relay_app.c`
- Create: `applications/main/ble_relay/ble_link.{h,c}` (NimBLE central+peripheral bring-up, coex-enabled)

- [ ] **Step 1:** Bring up a NimBLE peripheral (dummy 1-char service) AND ESP-NOW on the same device; confirm both operate (advertise seen + ESP-NOW echo works) with coex enabled.
- [ ] **Step 2:** Measure the added round-trip of `GATT write → ESP-NOW → echo → GATT notify` vs the ESP-NOW-only number from 0.1; draw the delta + jitter on screen.
- [ ] **Step 3:** Record the number in the plan + memory. **GATE:** if added latency is prohibitive for timed targets, mark the tool bench-only in-UI and note it; proceed to Phase 1 regardless (untimed targets still in scope).
- [ ] **Step 4:** Build/gate/commit as in 0.1.

---

## Phase 1 — GATT clone / enumerate + faithful impersonation

### Task 1.1: Central enumerate a target's GATT profile

**Files:** Modify `ble_link.{h,c}`, `ble_relay_app.c`; Create `ble_relay/gatt_profile.{h,c}` (captured service/char table + save/load to `/ext`).

- [ ] **Step 1:** Central connects to an owner-controlled BLE device, walks services→characteristics→descriptors, stores UUIDs + properties in `gatt_profile`.
- [ ] **Step 2:** Persist the captured profile + advertisement payload + identity to `/ext/ble_relay/<name>.profile`; reload round-trips (host-check the serializer via a Python replica).
- [ ] **Step 3:** Draw the enumerated services/char count; build/gate/commit.

### Task 1.2: Peripheral reproduces the captured profile

**Files:** Modify `ble_link.{h,c}`, `gatt_profile.{h,c}`.

- [ ] **Step 1:** Peripheral registers the captured GATT services/chars + advertises the captured name/UUIDs + (where legal/possible) identity.
- [ ] **Step 2:** Verify a phone/central connects to the impersonating peripheral and sees the cloned profile (manual, on-device). Note RPA/rotating-MAC limitations per target.
- [ ] **Step 3:** Build/gate/commit.

---

## Phase 2 — Single-link GATT proxy (prove app-layer survives two hops)

### Task 2.1: Forward one characteristic write→notify across ESP-NOW

**Files:** Modify `ble_relay_app.c`, `ble_link.c`, `espnow_link.c`; Create `ble_relay/relay_core.{h,c}` (op mux: BLE-op ⇄ ESP-NOW frame).

**Interfaces:** `relay_core_on_ble_op(op)` → frames + sends; `relay_core_on_espnow_frame(frame)` → replays as BLE op. Roles selectable (key-side / car-side) at runtime.

- [ ] **Step 1:** Pure `relay_core` op↔frame mapping (seq/ack, chunk reassembly) — host-tested via a Python replica covering out-of-order + fragmented ops.
- [ ] **Step 2:** Wire key-side: a GATT write from the phone-side central is framed → ESP-NOW → car-side replays it as a write to the real target; the target's notify returns the same path. Prove an app-layer challenge/response byte-string round-trips against a controlled target.
- [ ] **Step 3:** Draw per-op latency; build/gate/commit.

---

## Phase 3 — Full 2-device relay (GATED on Phase 0 result)

Detailed tasks written after Phase 0's latency number is known (it decides bench-only vs real-range, and whether seq/ack needs a latency budget). Scope: both roles concurrently, all GATT ops (read/write/notify/indicate), reconnect handling, dual-screen status/latency banner, authorized end-to-end test vs the owner's own lock/vehicle, mirror to `src_cardputer_adv`.

## Phase 4 — Robustness (GATED)

RPA / identity-rotation handling, multi-target profile store, MTU negotiation, honest in-UI scope/latency banner finalized, guardrail re-audit (`nm` denylist for BLE-relay symbols in public).

---

## Self-review

- Every Phase 0–2 task ends in an independently testable deliverable (replica PASS, on-device observation, or `nm`/gate check).
- No host gcc → verification is Python-replica + xtensa syntax-check + `nm` + on-device, matching this repo's other plans.
- Phases 3–4 are deliberately outlined-not-detailed because their scope is gated on the Phase 0 measurement — elaborate them once that number exists.
