# Unified "Car Keys" RKE Mode — Design Spec

**Date:** 2026-08-03
**Status:** Design (pending user review)
**Boards:** `src_m5stick_s3` + `src_cardputer_adv` (private builds only)
**Branch:** continues `feat/keeloq-no-desync-clone`
**Builds on:** [[keeloq-no-desync-clone]] (clone + fob emulator), the unified cars registry, `vehicle_vuln_db`, the rolling analyzer.
**Scope framing:** own-vehicle / authorized security testing only. Dual-use; private-only.

## Goal

Tie the pieces that already exist — the Cars decoder registry, `vehicle_vuln_db`, the rolling-code analyzer, the KeeLoq no-desync clone, and the fob emulator — into **one guided RKE workflow**: enter Car Keys → (auto-configured RX) capture → **identify make + vuln** → act (Clone→Fob / Analyze / Save→Garage), with a persistent **garage** of saved fobs. Also surface the same vehicle card from the normal Read flow.

## What already exists (reuse, don't rebuild)

- **Unified Cars registry:** `subghz_protocol_registry_set_car_mode(1)` enables the merged automotive decoder set; `car_mode` is 0/1 (`protocol_items.c:103-115`), toggled today only via Radio Settings "Automotive: Off/Cars".
- **Vuln DB:** `vehicle_vuln_db.{c,h}` — 21 CVE entries `{cve, year_start, year_end, makes[], models[], region, description, url}`, already browsable via `subghz_scene_vuln_makes.c` / `subghz_scene_vuln_list.c`. It is a **CVE reference keyed by make**, NOT a signal fingerprinter.
- **Rolling analyzer + Clone + Fob:** `subghz_scene_rolling_analyzer.c` (can_clone gate, Clone action) → `subghz_scene_keeloq_fob` (via the clone build). `subghz_txrx_gen_keeloq_clone` writes a `CloneMode` `.sub`; the encoder deserializes `CloneMode` and resumes the counter.
- **Entry menu:** `subghz_scene_start.c` (Read / Read RAW / Frequency Analyzer / …).

## Confirmed decisions

1. **Entry: Both** — a top-level "Car Keys" menu item AND the vehicle card also shown from the normal Read flow.
2. **Rich vehicle card, make-accurate** — make from the decoded protocol/manufacturer; exact model NOT pinned from RF; the card cross-references `vehicle_vuln_db` by make to show a CVE/RollBack badge + candidate vulnerable models; plus protocol, rolling/fixed, cloneable, and actions.
3. **Persistent Garage** — a saved fob library under `/ext/subghz/garage/`; Save writes a clone `.sub`, My Garage lists + re-opens them (counter resumes). This also delivers the previously-deferred "Save fob".

## Design

### A. Entry & navigation

- **Top-level menu:** add `"Car Keys"` to `subghz_scene_start.c` → `SubGhzSceneCarKeys` hub.
- **Car Keys hub** (`subghz_scene_car_keys.c`) — a submenu:
  - **New Capture** → auto-config then RX: `subghz_protocol_registry_set_car_mode(1)`, apply the car frequency set (315.00 + 433.92 hopping via the existing hopper/freq config), then `scene_manager_next_scene(SubGhzSceneReceiver)`.
  - **My Garage** → `SubGhzSceneGarage` (list of saved fobs).
  - **Vuln DB** → the existing `SubGhzSceneVulnMakes` browser.
- **Restore on exit:** remember the prior `car_mode`/frequency and restore on leaving Car Keys (don't leave the user's normal RX reconfigured). Store the previous values on hub enter; restore on hub exit.

### B. Vehicle card

A card scene (`subghz_scene_car_card.c` + a small widget/view) shown when a **car-class** capture is opened — both from the Car Keys receiver and from the normal Read → received-signal path (for car protocols only; non-car signals keep today's info screen).

Card contents:
- **Make** — from `car_make_from_decode()` (new helper): for native car decoders the protocol name maps to a make (e.g. "Honda"/"Hyundai TL"→"Hyundai"); for KeeLoq the matched `manufacture_name`. Returns a display make string (or "Unknown").
- **Protocol + rolling/fixed** — from the decoder (protocol name; rolling if dynamic/KeeLoq, fixed otherwise).
- **Cloneable: Yes/No** — the existing `can_clone` predicate (KeeLoq + known mfr + recovered counter; excludes Unknown/BFT/Erreka/AN-Motors).
- **Vuln cross-reference** — `vehicle_vuln_find_by_make(make)` (new helper over `vehicle_vuln_db`): if the make has entries, show a badge (CVE id + "RollBack") + the union of that make's `models[]` as "known-vulnerable models" (candidates, explicitly not a model pin). If no match, no badge.
- **Actions:** `[Clone→Fob]` (→ build clone jump 100 → `SubGhzSceneKeeloqFob`, the existing path), `[Analyze]` (→ `SubGhzSceneRollingAnalyzer`), `[Save→Garage]` (writes the clone `.sub` to the garage). Clone/Save shown only when `can_clone`.

### C. Garage (persistent fob library)

- **Location:** `/ext/subghz/garage/` (created on first Save).
- **Save:** build the clone into `fff_data` (`subghz_txrx_gen_keeloq_clone`, jump 100) if not already built, then write it to `garage/<Make>_<serial8hex>.sub` via the standard FlipperFormat save (mirror `SubGhzSceneSaveName`'s writer, but auto-named + fixed folder, no name prompt). The `.sub` is a normal `CloneMode` clone file.
- **My Garage** (`subghz_scene_garage.c`): list `*.sub` in the garage folder (name + make). Selecting one loads it (the encoder's `CloneMode` deserialize) and routes to `SubGhzSceneKeeloqFob` with the counter resumed. Long-press / a Delete action removes a file.
- **Counter persistence:** already handled by the clone engine — each send rewrites `Cnt` in the loaded file's `fff_data`; on exit, the file's `Cnt` is written back so re-opening resumes forward. (Add the write-back-to-file on fob exit if not already persisted to disk — confirm during planning; the in-RAM `fff_data` persist exists, disk write-back on Save/exit is the new bit.)

### D. Auto-config details

- **Car frequencies:** enable a hopping set of the RKE-relevant frequencies already supported (315.00 MHz NA + 433.92 MHz EU at minimum; reuse the existing hopper + user frequency list rather than hardcoding). Confirm the exact frequency-set API in planning.
- **Decoders:** `set_car_mode(1)` before building the receiver so the automotive registry is active.

## Components & boundaries

**Create (both trees):**
- `applications/main/subghz/scenes/subghz_scene_car_keys.c` — hub submenu + auto-config/restore.
- `applications/main/subghz/scenes/subghz_scene_car_card.c` — the vehicle card (+ a widget or reuse `widget`/`dialog_ex`).
- `applications/main/subghz/scenes/subghz_scene_garage.c` — saved-fob list + open/delete.
- `applications/main/subghz/helpers/subghz_car_id.{c,h}` — `car_make_from_decode()` + `vehicle_vuln_find_by_make()` (pure-ish lookup helpers; the make-mapping table is host-testable).
- Register all three scenes in `subghz_scene_config.h`.

**Modify (both trees):**
- `subghz_scene_start.c` — add the "Car Keys" menu item + route.
- `subghz_scene_receiver_info.c` (or the received-signal action path) — for car-class protocols, route the "ID"/open to `SubGhzSceneCarCard` instead of the generic info; non-car unchanged.
- `subghz_i.h` — a small saved-state field for the prior car_mode/freq (restore) + any garage path constant.
- Guard all new scenes/helpers + the menu item with `#ifndef PUBLIC_BUILD` (car decoders are already private-only).

**Reuse unchanged:** rolling analyzer, KeeLoq clone, fob emulator, `vehicle_vuln_db` + its browser scenes, the cars registry, the receiver scene.

## Data flow

Car Keys → New Capture → (car_mode=1 + car freqs) → Receiver RX → car protocol decodes → open → **Car Card** (make via `car_make_from_decode`, vuln via `vehicle_vuln_find_by_make`, cloneable via `can_clone`) → `[Clone→Fob]` (existing clone→fob) or `[Analyze]` or `[Save→Garage]` → Garage persists the `.sub` → My Garage re-opens → Fob (counter resumes).

## Error handling / edge cases

- **Unknown/non-clonable capture:** card still shows make + protocol; Clone/Save hidden (can_clone false); Analyze still offered.
- **Make not in vuln_db:** no badge (common — most makes aren't in the 21-entry DB); card is still useful.
- **Model is never pinned from RF** — always presented as "vulnerable models (candidates)", never as a definitive identification. Do not claim exact model.
- **Garage folder missing / SD absent:** create on first Save; if SD write fails, show the standard "requires SD" error.
- **Restore radio state** on leaving Car Keys so the user's normal Read isn't left in Cars/hopping mode.
- **Naming collisions in garage:** if `<Make>_<serial>.sub` exists, append `_2` etc. (or overwrite the same fob — same serial = same fob = overwrite/resume is correct). Prefer overwrite-same-serial so re-saving a fob updates its counter rather than duplicating.

## Testing

- **Host unit test:** `car_make_from_decode()` mapping (protocol/mfr → make) and `vehicle_vuln_find_by_make()` (make → entries) via WSL gcc.
- **Compile gate:** `build_ci.bat` per tree; public build compiles with Car Keys guarded out.
- **On-hardware (both boards):**
  1. Main menu → **Car Keys** → New Capture → capture a KeeLoq car fob → **vehicle card** shows make + cloneable (+ vuln badge if the make is in the DB).
  2. **Clone→Fob** opens the fob emulator (as today).
  3. **Save→Garage** → **My Garage** lists it → re-open → fob emulator, counter resumed and advanced from last session.
  4. Normal **Read** (Cars on) → open a car capture → same card appears.
  5. Leaving Car Keys restores the prior radio/decoder state.

## Non-goals

- The **RollBack / Rolling-PWN / Hitag-2 attack implementations** (features B / C / D) — v1 shows the vuln badge (informational) and Clone covers the known-key path only.
- **Exact-model identification** from RF (not possible; make + candidate models only).
- No new crypto; no public/hardened exposure.

## Rollback

Additive: new scenes/helpers + a menu item + a conditional route in receiver_info. Reverting the commit restores the menu and the generic info screen; the clone/fob/analyzer paths are untouched.
