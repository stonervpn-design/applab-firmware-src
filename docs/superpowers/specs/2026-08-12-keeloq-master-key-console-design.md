# KeeLoq Master-Key Console — design spec

**Date:** 2026-08-12
**Status:** Approved design, pending implementation plan
**Scope:** PRIVATE build only (car / master-key attack surface). Public build unaffected.
**Tree:** `src_m5stick_s3` (superset), boards m5stick_s3 + m5stack_cardputer_adv.

## Goal

Make every plaintext KeeLoq manufacturer key in the private keystore
(`keeloq_mfcodes_user`, 216 entries / 176 unique) usable end-to-end —
**identify (attack) → generate/emulate → clone/predict** — from one coherent
place, and prove that coverage with a host round-trip test. Close the real gaps
(a picker allow-list that drifted behind the encoder; fragmentation across three
menus) without rewriting any working protocol.

## Background — current state (measured)

The 216 plaintext entries use a `key:type:name` format where **`type` is the
KeeLoq learning type AND a de-facto protocol tag**. Each dedicated protocol
pulls its key from the *same* keystore by matching that type
(`jarolift.c` matches `KEELOQ_LEARNING_NORMAL_JAROLIFT`==11,
`kinggates_stylo_4k.c` matches `KEELOQ_LEARNING_SIMPLE_KINGGATES`==10, FAAC SLH
by name+type). So routing by learning type is exact, not heuristic.

Coverage of the 216 today:

| Bucket | Types | # keys | Decode | Encode | Emulatable now |
|---|---|---:|:--:|:--:|:--:|
| Generic KeeLoq, fully works | 1,2,3,4,6,7,8 | 143 | ✅ | ✅ | ✅ via KeeLoq "By Manufacturer" picker |
| Encoder-ready but **hidden** | ERREKA 12, PUJOL 13, AERF 14, SIMPLE_JCM 15 | 36 | ✅ | ✅ (implemented) | ❌ excluded by `type_encodable()` allow-list |
| Dedicated protocols | FAAC 5, KingGates 10, Jarolift 11 (incl. KIA V3&4/V6 by name) | 9 | ✅ (own decoder) | ✅ (own Gen flow) | ✅ but via a *different* menu (SetType presets) |
| Generic decode keys | UNKNOWN 0 | 25 | ✅ | partial (needs a chosen `kl_type`) | ❌ not tied to one manufacturer |
| Type-9 (unrouted / car) | type 9 | 3* | ❌ | ❌ | ❌ no decoder consumes type 9 |
| **Total** | | **216** | | | |

\* The three type-9 entries are `KIA_Hyundai_V5` ×2 and `Beninca_ARC`. The plan
resolves each: the KIA rows likely belong to the Kia V5 car protocol; the Beninca
row is genuinely unrouted (mark decode-only or confirm a decoder). Note KIA also
appears under types 10/11 (V3&4, V6) — those rows are counted in the dedicated
bucket and reached via the Kia generators.

**Root problems, not missing ciphers:**
1. The mfr picker's hardcoded `subghz_scene_keeloq_mfr_type_encodable()` allow-list
   lists SIMPLE/NORMAL/SECURE/MAGIC_XOR/MAGIC_SERIAL{1,2,3} but omits
   AERF/ERREKA/PUJOL/SIMPLE_JCM — even though the encoder
   (`subghz_protocol_keeloq_gen_data`) already has branches for them. **36 keys
   are hidden by drift.**
2. The 216 keys are reachable only across **three unrelated menus** (KeeLoq By
   Manufacturer, the FAAC/Jarolift/KingGates SetType presets, and the car
   protocols). No single place identifies/owns an arbitrary KeeLoq fob.
3. No explicit, user-visible **known-key dictionary attack** on a capture.

**Cryptographic reality (drives the "attack" scope):**
- **Known-key** clone/emulate/predict is feasible and mostly built.
- **Unknown-key** exhaustive recovery is 2⁶⁴ — **not feasible on-device or off**.
  The only real unknown-key path is the academic correlation/slide attack
  (Bogdanov/Indesteege), which needs many chosen captures + hours–days on a PC →
  **host-side only, out of the on-device scope** (optional stub noted, not built
  here).

## Non-goals (YAGNI)

- No reimplementation of FAAC/KingGates/Jarolift/Kia ciphers inside `keeloq.c`
  (Approach C — rejected: rewrites working code, redundant).
- No on-device unknown-key brute force (infeasible; would be a fake progress bar).
- No new radio/TX stack — TX stays on the standard txrx encoder path.
- Public build gains nothing (all new UI is `#ifndef PUBLIC_BUILD`).

## Architecture

Two modes over one shared **registry**, reusing existing scenes:

```
            KeeLoq Master-Key Registry  (new helper — single source of truth)
            per keystore entry: {name, key, type, family, encodable, decode_only}
                       │
        ┌──────────────┴───────────────┐
   ATTACK (from a capture)         CATALOG (from scratch)
   identify → clone/emulate/predict     browse all 216 → correct generator → emulate/save
```

The registry is a thin *view* over the once-loaded keystore (no key copies): a
`type → {family, generator, capability}` table plus accessors. Both modes read
it, so coverage is defined in exactly one place and cannot drift again.

## Components

### 1. KeeLoq Master-Key Registry — `helpers/subghz_keeloq_registry.{c,h}` (new)
- **Input:** the parsed keystore (`SubGhzKeyArray_t` = `{key, type, name}`).
- **Core:** static `type → {family, generator, capability}` table.
  `family ∈ {GenericKeeLoq, FaacSLH, KingGates, Jarolift, KiaCar}`.
  `capability ∈ {FullEmulate, DecodeOnly, GenericPickMode}`.
- **API:**
  - `size_t subghz_keeloq_registry_count(SubGhz*, RegistryFilter)`
  - `bool subghz_keeloq_registry_entry(SubGhz*, size_t i, RegistryFilter, KeeloqRegEntry* out)` → `{name,key,type,family,encodable,decode_only}`
  - `KeeloqGen subghz_keeloq_registry_route(uint16_t type, const char* name)` → generator enum
  - `bool subghz_keeloq_registry_type_encodable(uint16_t type)` (replaces the mfr scene's local copy)
- **Guarantees:** pure/table-driven (host-compilable, no furi UI deps in the
  mapping core); the +36 formerly-hidden types return `encodable=true`.

### 2. Catalog console scene — `scenes/subghz_scene_keeloq_console.c`
Generalizes `subghz_scene_keeloq_mfr.c`:
- Lists every registry entry with a generate route, grouped by family with a
  count header (216-item usability). Type-0 under a "Generic KeeLoq (choose
  mode)" group; unrouted type-9 shown as "decode-only".
- On select → `registry.route()` → the matching generator via the existing
  SetSerial → Gen{Keeloq|FaacSLH|KingGates|Jarolift|Kia} → Emulate/Save flow,
  setting `keeloq_mfr_emulate_flow`.

### 3. Attack path — extend `scenes/subghz_scene_analyze.c` (Signal-ID) + rolling analyzer
- On a decoded rolling capture: registry names manufacturer + family; expose
  Clone (no-desync) / Emulate (grid) / Predict-next.
- Explicit **Dictionary attack** action: run all 216 × types against the
  captured hop, report the manufacturer key that yields a valid decrypt
  (serial/checksum match) + confidence. Reuses the decoder's key loop, surfaced
  as a first-class action for captures that did not auto-decode.

### 4. Allow-list removal — `scenes/subghz_scene_keeloq_mfr.c`
Delete the local `type_encodable()`; call `subghz_keeloq_registry_type_encodable()`.
Single gate → no future drift.

### 5. Host coverage test — `tools/decoder_host_test/keeloq_coverage.c` (+ run script)
For each registry entry: gen(key, fixed serial, btn, counter N) → encoder →
decoder → assert (manufacturer resolves; serial/btn/counter round-trip; counter
advances N→N+1). Emits a **216-row PASS / DECODE-ONLY / ORPHANED** matrix with a
reason per non-PASS row.

### 6. Honest-limits handling (data on the registry entry, surfaced in UI)
- `decode_only` (type-0, 25): catalog "choose mode" prompt (Simple/Normal/Secure)
  before generate; labeled.
- `orphaned` (type-9 non-KIA): shown "decode-only (unrouted)"; never silently TX'd.
- Unknown-key: dictionary no-match screen states 2⁶⁴ infeasibility + points at the
  host correlation tool. No on-device brute UI.

## Data flow

**Attack (capture):** RX → decoder tries keystore keys by type → decoded frame →
receiver-info "ID" → Signal-ID hub (registry names it; optional Dictionary-attack
action) → Clone / Emulate / Predict-next.

**Catalog (scratch):** KeeLoq Console → registry lists 216 grouped → select →
`route()` → SetSerial byte-input → correct Gen flow → Emulate (fob grid) or Save.

Shared contract: both read `registry.entry(i)` / `route()`; registry is a view
over the once-loaded keystore (no key copies).

## Error handling

- No keystore → "add `keeloq_mfcodes_user` to SD" (existing behavior).
- Dictionary no-match → honest 2⁶⁴/host-tool message; no fake progress bar.
- Registry gates encodability → a non-encodable entry can never reach a broken
  generator (this is the exact silent-invalid-TX the current picker guards, now
  centralized).

## Testing

1. **Host coverage** (`keeloq_coverage.c`): 216-row round-trip matrix; gate = all
   `encodable` entries PASS; decode-only/orphaned rows documented with reasons.
2. **Registry unit test** (`tests/test_keeloq_registry.c`, WSL gcc): type →
   family/route/encodable correct for representative types (0,1,2,5,9,10,11,14,15);
   count matches keystore; the +36 formerly-hidden types now `encodable=true`.
3. **Dictionary-attack test**: seeded known (key,serial,counter) frame → identifies
   the right manufacturer; rejects a wrong-key frame.
4. **No-regression**: existing 143-key picker + FAAC/Jarolift/KingGates gen flows
   still work; VAG/KeeLoq fob emulate unchanged; private m5stick + cardputer +
   public builds green; public bin passes the no-car verify.

## Success criteria

- All **encodable** registry entries — the **179 generic** (143 already-working +
  36 un-hidden) plus the **9 dedicated-protocol** keys reached through the console
  (≈188 of 216) — generate a valid frame that decodes back with the correct
  manufacturer, serial, button and an advancing counter, proven by the host matrix.
  The remaining 28 (25 type-0 generics + 3 type-9) are covered as documented
  decode-only/choose-mode rows, not silent gaps.
- Every one of the 216 is reachable from the console (emulate route, or an explicit
  decode-only/orphaned label — nothing silently missing).
- A captured known-manufacturer KeeLoq fob can be identified, cloned, emulated and
  counter-predicted from the Signal-ID hub.
- No regression to existing flows or the public build.

## Open items for the plan

- Resolve each **type-9** row's route (KIA_Hyundai_V5 → Kia V5 car protocol?;
  Beninca_ARC → confirm decoder or mark decode-only).
- Decide the console's **menu placement** (Car Keys hub vs Read/Add-Manually).
- Confirm whether **type-0 emulate** (choose-mode) is in v1 or deferred (decode/attack
  only in v1).
