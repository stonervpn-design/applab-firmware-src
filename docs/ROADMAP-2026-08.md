# Private Ports — Enhancement Roadmap (2026-08)

Grounded in a 7-subsystem survey of the actual codebase. Effort = S/M/L/XL, risk = low/med/high.

## State of the ports (honest)

**Strong / mature:** the WiFi·BLE·2.4 GHz suite is the most exercised code you have (Evil Portal, deauth, PMKID/handshake capture, BLE spam/GATT-walk/tracker, nRF24 jammer+Mousejack, Camera Hunter, wardriver/pwnagotchi — all code-complete). The RF crypto **cores** are trustworthy (Hitag-2 cipher byte-exact vs MIKRON; KeeLoq desync-safe clone; rollback store — all host-tested). IR (RMT HAL), BadUSB (full DuckyScript, USB+BLE), and the 3-backend NFC HAL (ST25R3916/PN532/WS1850S) are solid. The public↔private guardrail is freshly hardened.

**Fragile / unproven:** a large share of the car/RF *attack* features are **"compiles-untested"** — the logic is proven but the on-RF/real-device behavior isn't: Hitag-2 on-device TX (sim-only), the desync clone end-to-end, the jammer, Rolling-PWN replay, Flipper Share, nRF24 Mousejack, magic-card write. Several `furi_crash("Not implemented")` UI dead-ends exist.

**The 3 structural risks that make everything else riskier:**
1. **Source-tree drift.** `src_m5stick_s3` (canonical) vs `src_cardputer_adv` vs a lilygo tree — ~99 files diverge; the cardputer tree is **missing `verify_public_no_car.py` entirely** and the newest `hitag2_cipher.c`. Every fix must be hand-applied 2–3× and silently rots on the tree that's missed.
2. **CI is broken *and* unsafe.** `build.yml` crashes on its board-key guard for every job, builds the wrong boards, runs no host tests, no car-free gate — and would upload **full private car-material artifacts by default** (variant defaults to private).
3. **The applab cloud build publishes to strangers with NO car-free gate.** `custom-build.yml` forces public but never runs `verify_public_no_car.py` before pushing to the public repo. The guardrail is enforced only on your local `.bat` path.

Plus: the guardrail relies on **4 hand-maintained parallel car lists** (a new protocol can slip if one is missed), and the crown-jewel cipher/clone **host tests are orphaned** (nothing compiles/runs them).

---

## Phase 0 — Quick wins (all S, ~a week, close real holes)

| # | Item | Why | Area |
|---|------|-----|------|
| 0.1 | **Wire `verify_public_no_car.py` into `custom-build.yml`** (before publish) | Closes the *live* guardrail hole — the only path handing bins to strangers has no gate | security |
| 0.2 | **Give the 40 host tests a runner + a CI job** (`tests/Makefile` + fast job) | Crown-jewel cipher/clone tests exist but nothing runs them | test |
| 0.3 | **Fix dead sdkconfig symbols** (cardputer BT tuning is inert under NimBLE; `ESP_SYSTEM_ABORT_ON_ALLOC_FAILURE` wrong name) | Same bug class already cost ~9.6 KB WiFi RAM | RAM |
| 0.4 | **Build-time assert `CONFIG_SPIRAM=n`** for no-PSRAM boards | Converts the recurring PSRAM-pollution boot-loop into a caught build error | reliability |
| 0.5 | **Replace `furi_crash("Not implemented")` dead-ends with no-ops** | Reachable UI paths that hard-crash on RAM-tight boards | reliability |
| 0.6 | **Drift-guard script** (diff shared files across trees; fail on divergence of `verify_public_no_car.py`, `fam_config.py`, protocols) | Makes drift loud until unification lands | tooling |
| 0.7 | **key_copier/TPMS bare-brand allowlist** → re-enable bare-brand scanning | Turns the documented scanner blind spot into an explicit allowlist | security |
| 0.8 | **Persist ccache in CI + standardize `inc_build`** | Cold 3-board builds dominate; ~2× faster | perf |
| 0.9 | **Enable U2F** (dormant, fully implemented, HID already works) + **IR `/ext` DB loading** (finish the TODO) | Near-zero-cost capability wins | capability |
| 0.10 | **Delete dead scripts** (diag2/3, clean_build_jul12, build_fix, test_build) + resolve `car_protocol_files.txt` | Cleanup / remove footguns | hygiene |

---

## Track A — Foundations (do before big feature work; makes everything 2–3× cheaper)

- **Collapse the 2–3 source trees into ONE board-parameterized tree** — `L`/med. The machinery already exists (`FLIPPER_BOARD`, per-board `sdkconfig.defaults.<board>`, `targets/<board>/`). Root cause of most future regressions. *Depends on: diff the ~99 files first, fold real deltas under `targets/<board>/`.*
- **Fix + repurpose GitHub Actions to the real board×variant matrix** — `M`/med. Invoke `idf.py` directly (like `custom-build.yml` does), run host tests + the car-free gate on every public bin. Turns CI from broken-and-unsafe into a real gate.
- **Golden-vector decoder regression harness** — `M`/low. Corpus of real RAW `.sub` per protocol → assert decoded fields. Catches the exact regressions you've already hit (Ford V0 CRC, Toyota FSK-as-OOK).
- **Collapse the ~20 `.bat` scripts into one `build.py --board --variant`** — `M`/med. Kills copy-paste drift, keys off exit codes not `findstr SENTINEL_OK`.
- **Reproducible versioned releases** — `M`/low. Git tag → GitHub Release with per-board bins + SHA256SUMS + changelog; embed git-sha in the About screen.

## Track B — Guardrail hardening (so the leak class can't recur)

- **Auto-derive all 4 car lists from ONE source** — `M`/med. Generate the CMake EXCLUDE regex + `CAR_NAME_MARKERS` from the `protocol_items.c` automotive registry; CI diff-fails if hand-edited. Makes "new car protocol can't leak" *structural*.
- **CI public⊄private symbol/string set-diff gate** — `M`/med. Build both, assert no private-only string/symbol appears in public. Catches leaks the denylist can't.
- **Threat-model doc + PR checklist** — `S`/low. `THREAT_MODEL_PUBLIC.md` + a "touched car/keystore/clone? confirm guard+marker+exclusion" checkbox.
- **Sign/attest public bins** — `M`/low. SHA + minisign in the builds repo; webflasher verifies.
- **Consolidate the 23 app-side `#ifndef PUBLIC_BUILD` guards** — `L`/med. Fewer choke points = fewer to forget.

## Track C — Reliability & RAM (make "runs everything smoothly" true)

- **Coredump readback + symbolication pipeline** — `M`/low. CLI `coredump` dump/erase + host `decode_coredump.py`. **Unblocks the two open crashes** (WiFi-entry OOM, IR-learn-close) that are stuck for lack of a backtrace. Capture is already configured.
- **Heap/DMA budget tool + CI floor gate** — `M`/low. `ram_budget.py` off the `.map`; fail build below a per-board free-internal floor. Turns "we think ~103 KB free" into an enforced number.
- **Systematic on-demand-buffer + alloc NULL-check pass** — `L`/med. Rank fattest `.bss`, convert to on-demand heap, NULL-check entry allocs so OOM degrades instead of crashing.
- **Instrument + validate DFS power benefit** — `M`/low. Measure the actual mA saved; the design is sound but the payoff is unmeasured (panel may dominate).
- **2-button Left/Right access path** — `S`/low. A chord/double-click so d-pad apps become navigable on the M5Stick.

## Track D — RF research depth (your core interest)

- **On-device modulation auto-detect in the frequency analyzer** — `L`/med. Classify OOK vs 2-FSK/GFSK after freq lock, auto-pick the CC1101 preset. **Biggest capture-quality win** — directly fixes the "FSK-as-OOK garbage" / below-noise-floor failures.
- **Capture-quality meter + RSSI/preamble gating** — `M`/low. Live quality indicator; block saving garbage `.subs`.
- **On-device known-key Hitag-2 fresh-counter forge** — `M`/med. Port `hitag2_make_forge.py` on-device (cipher already byte-exact) — replay→real forge.
- **Make-specific Rolling-PWN / RollBack executor** — `M`/med. Honda-class consecutive-counter window; the generic path "never fires for Honda" today.
- **Jammer: CW-carrier + band-sweep + reactive modes** — `M`/med. Today it's one fixed freq, OOK noise only.
- **TPMS emulate + POCSAG TX + modern-Toyota wide-FSK** — `M`/`L`. Two-way TPMS testing; pager research; close the modern-Toyota decode gap (*after* modulation auto-detect).

## Track E — NFC breadth (badge/access research)

- **NFC-B / ISO15693(SLIX) / Felica on ST25R3916** — `L`/med. Chip supports them; only NFC-A is wired. **Unlocks a dozen dead-but-compiled protocols + parsers** in one move.
- **On-device MIFARE Classic nested attack** — `L`/med. `crapto1` already in-tree; today a card with no known key + no reader is uncrackable. Biggest badge-cloning gap.
- **Full MFC/Ultralight/NTAG emulation on ST25R3916** — `XL`/high. The clone→emulate endgame; listener + Crypto1 primitive already exist for reads.
- **Validate/harden magic-card write over ST25R3916** — `M`/med. PN532-era code; verify Gen1a/Gen4 pass the custom-parity path + readback-verify.
- **Finish Flipper Share on-device acceptance + polish** — `M`/low. Code-complete, never HW-validated; a genuinely novel flagship once tested.
- **Backend-aware capability UI** — `S`/low. "Needs ST25R3916 unit" instead of silent timeouts.

## Track F — New capabilities / 2.4 GHz (leverage existing strength)

- **Deauth → 4-way-handshake → hashcat-22000 auto-capture in one scene** — `M`/low. All pieces exist separately; the single most-wanted WPA workflow.
- **Drone / RemoteID detection in Defender** — `M`/med. `DefenderCatDrone` is advertised but never raised; parse ASTM F3411 off the existing beacon ring + BLE adv path.
- **Finish Defender PCAP evidence logging** — `S`/low. `_pcap_*` are no-op stubs; port `wlan_pcap.c`.
- **Unified 2.4 GHz recon dashboard** (WiFi+BLE+nRF24+ESP-NOW occupancy) — `L`/med. Also lowers peak RAM by consolidating promiscuous callbacks.
- **ESP-NOW TX** (spoof/replay/flood) + **BLE GATT write** + **Tesla BLE scanner port** — `S`–`M`. RX-only today; the injection HAL already exists.
- **Enable SubGHz Remote (.sub playlist) / USB-BT HID app / ESP-NOW app** — `M` each. Dormant/complete apps not in the base APPS list.
- **RF-triggered BadUSB (cross-domain macros)** — `L`/high. RX code → run payload; combines your 3 strongest subsystems into something stock Flipper can't do.
- **Port the JS bus modules (serial/gpio/i2c/spi)** — `L`/med. Turns the device into a scriptable bench tool.

---

## Cross-cutting must-dos (underpin everything)

1. **Unify the trees (or at least the Phase-0 drift-guard) before big feature work** — otherwise every feature below is done 2–3×.
2. **Fix + gate CI (car-free + host tests) before any more public builds.**
3. **One on-hardware validation session** for the built-but-untested car/RF features (desync clone, Hitag-2 TX/forge, Rolling-PWN, Mousejack, magic write, Flipper Share) — most of the "compiles-untested" status collapses to "works" or "here's the real bug" in one focused pass.

## Recommended sequence

- **Phase 0 (now):** the quick-wins table — safety + hygiene, ~a week.
- **Phase 1 (foundations):** unify trees → real CI matrix + host-test/car-free gates → golden-vector harness → guardrail auto-derive. Everything downstream gets cheaper and safer.
- **Phase 2 (reliability):** coredump pipeline → root-cause the 2 open crashes; RAM budget tool + on-demand pass; 2-button Left/Right.
- **Phase 3 (pick your lane):** RF depth (modulation auto-detect + Hitag-2 forge + Rolling-PWN) **or** NFC breadth (ISO15693 + nested + emulation) **or** 2.4 GHz (handshake pipeline + drone detect + recon dashboard) — interleaved with the HW-validation session.
