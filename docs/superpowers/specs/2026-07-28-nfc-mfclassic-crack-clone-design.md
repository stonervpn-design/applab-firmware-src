# NFC: MIFARE Classic crack → clone loop (ST25R3916) — Design Spec

**Date:** 2026-07-28
**Board:** `m5stick_s3` (ESP32-S3-PICO-1-N8R8: 8 MB flash **+ 8 MB PSRAM**), tree
`_fzcard_work/NEW_v2/src_m5stick_s3`. Primary NFC reader = built-in **ST25R3916**
(M5 NFC2 @ Grove I2C 0x50); optional **PN532** @ 0x24.
**Status:** Approved design — ready for implementation plan.
**Related:** `[[fzcard-st25r3916-nfc-backend]]`, prior `2026-07-10-nfc-mfkey-magic-design.md`
(mfkey32-by-emulation + magic write; this spec supersedes its "deferred nested/
hardnested/darkside" note now that a raw-capable ST25R3916 backend exists).

## 1. Purpose

Close the full pentest loop on MIFARE Classic 1K/4K: take an **unknown** card, recover
**all** sector keys **on-device** (no PC, no external `.fap`), read every sector, and
**clone** the dump — including block 0 / UID — onto a magic card (Gen1a backdoor / Gen2
direct), the magics fixed in the 28k/28l work. "Max out" = climb the entire reader-side
recovery ladder: dictionary → weak-PRNG & static nested → darkside → hardnested.

## 2. The enabling insight (why this is mostly *enabling*, not *inventing*)

The modern upstream Flipper nested/hardnested attack **already lives in the poller**:

- `components/nfc/protocols/mf_classic/mf_classic_poller.c/.h/_i.h` — full state machine:
  `MfClassicPollerModeDictAttackEnhanced`; `MfClassicNestedPhase{AnalyzePRNG, DictAttack,
  DictAttackVerify, DictAttackResume, Calibrate, Recalibrate, CollectNtEnc, Finished}`;
  `MfClassicPrngType{Unknown, NoTag, Weak, Hard}` (static-nonce cards are flagged
  separately via `static_encrypted` + backdoor `Auth3`, not a PRNG value); nested-nonce
  array + handlers
  `mf_classic_poller_handler_nested_collect_nt` / `..._collect_nt_enc` /
  `..._nested_calibrate`.
- `applications/main/nfc/lib/crapto1/crapto1.{c,h}` — **vendored** (`lfsr_recovery32`
  etc.); `applications/main/nfc/helpers/mfkey32_recover.c` — on-device recovery.
- `components/nfc/helpers/crypto1.c` — Crypto1 primitives.

**Today this ladder is gated to dictionary-only on this port.** It was written against the
**PN532**, whose `InDataExchange` cannot emit raw Crypto1 frames. The poller encodes that
limit directly (`mf_classic_poller.c` ~line 1932: *"on PN532 the raw-AUTH probe in
nested_collect_nt always fails (Crypto1 cannot be sent raw) … PN532 cannot perform the
nested attack"*) and bails to a **partial read** (dict keys only), regardless of backend.

**The ST25R3916 backend does not have that limitation.** Raw Crypto1 with encrypted
parity is exactly `st25r3916_hal_transceive_custom_parity` — already the mechanism behind
working MIFARE Classic read/write here. The auth path already routes correctly:
`mf_classic_poller_auth` → `iso14443_3a_poller_txrx_custom_parity`
(`iso14443_3a_poller.h:100`) → `furi_hal_nfc_iso14443a_poller_tx_custom_parity` →
(ST25R3916) `st25r3916_hal_transceive_custom_parity`. So the nested attack that is
impossible on PN532 is **available on the ST25R3916 backend** — the work is to stop
bailing on this backend, prove it on real cards, then add the two missing rungs.

## 3. The one honest hard wall

Crypto card-**emulation** stays impossible: the ST25R3916 is I2C-polled, so the ~87 µs
card-response deadline cannot be met (see `[[fzcard-st25r3916-nfc-backend]]`). Therefore
classic **`mfkey32` (capture a real reader's nonces by *emulating* the card)** is **not**
part of this feature. That is the *emulation* direction. **Every rung of this spec is
reader-side** (the device is the reader attacking a card it holds) and is unaffected.

## 4. Feasibility by rung (the timing nuance that drives phasing)

Weak-PRNG nested needs the **`Calibrate` phase** to measure nonce *distances* (`d_min`/
`d_max`) and predict plaintext nonces — this is **timing-sensitive** and the one real
unknown on an I2C-polled path (jitter vs nonce granularity). Static and hard attacks are
**timing-insensitive**:

| Rung | Needs known key? | Timing-sensitive? | Card class | Risk |
|---|---|---|---|---|
| Dictionary | no | no | any with default/known keys | works today |
| **Static nested** (static enc-nonce / backdoor Auth3) | ≥1 | **no** | Fudan/backdoor clones, many access cards | low — most robust rung on ST25R3916 |
| **Weak-PRNG nested** | ≥1 | **yes** (Calibrate) | classic weak-PRNG 1K/4K | **the Phase-1 unknown** — may need HAL FDT/block-tx timing tightening |
| **Darkside** | **no** | mildly | weak-PRNG, zero known keys | new code; recovers a first key |
| **Hardnested** | ≥1 | no | hard-PRNG (EV1 / Plus-SL1) | heavy compute+memory; PSRAM makes viable |

Phase 1 therefore proves **static nested first** (should work regardless), then weak-PRNG
nested (proves/repairs timing). If weak-PRNG timing proves unrecoverable on I2C, static
nested + dictionary + (Phase 3) hardnested still cover the ladder — weak-PRNG cards that
also expose a backdoor fall to static nested anyway.

## 5. Architecture

Enable the existing ladder on the ST25R3916 backend, guard the PN532 fallback, add the
missing rungs, and wire recovered keys into the read→magic-write clone. No re-architecture
of the poller FSM — it already models every phase.

### 5.1 Backend gate (foundational, Phase 1)
Replace the unconditional "PN532 can't do nested → partial" bail with a backend check.
Add a HAL capability query rather than leaking backend enums into the poller:

```
// furi_hal_nfc.h
bool furi_hal_nfc_supports_raw_crypto1(void);   // true on ST25R3916, false on PN532
```
- ST25R3916 branch: `return true;`  PN532 branch: `return false;`
- In `mf_classic_poller.c` AnalyzePRNG/NoTag handling: only take the partial-read bail when
  `!furi_hal_nfc_supports_raw_crypto1()`. When supported, proceed into the nested FSM.

### 5.2 Phase 1 — Weak-PRNG + static nested → clone
- Route `nested_collect_nt` / `nested_collect_nt_enc` / `nested_calibrate` through the
  already-wired custom-parity auth path; the only change is *not bailing* on a raw-capable
  backend (§5.1). Verify `iso14443_3a_poller_txrx_custom_parity` returns the encrypted
  nonce bytes intact on ST25R3916 (it already does for read/write; nested uses the same
  frames without CRC).
- **Timing:** if weak-PRNG `Calibrate` distances are unstable, tighten the auth turnaround
  via the existing block-tx FDT timer (`furi_hal_nfc_timer_block_tx_start_us`) so the
  nested auth is issued at a repeatable delay. Expose the calibration count / FDT as named
  constants for one-line on-device tuning. Static nested (no Calibrate) is the fallback.
- **Recovery solver:** reuse `crapto1` `lfsr_recovery32` (already vendored) for the
  weak/static nested key solve; confirm the poller's `NestedPhaseDictAttack`/`CollectNtEnc`
  path calls it on-device (not an offload). Recovered keys are added to the run's key set
  and the user dict (`mf_user_dict.c`).
- **Clone:** after keys are recovered and all sectors read, offer **"Write to magic card"**:
  Gen2 (direct block-0 write) and Gen1a (backdoor `0x40`/`0x43`, now working — 28k). Confirm
  whether `applications/main/nfc/scenes/nfc_scene_write.c` already writes block 0 / handles
  magic; if not, route the clone through the existing `nfc_magic` app pollers
  (`applications_user/nfc_magic/magic/protocols/{gen1a,gen2}`) via a "Save & Clone" action.

### 5.3 Phase 2 — Darkside
- Port the darkside attack (reader sends auth with a fixed nonce and exploits the 4-bit NACK
  parity leak to recover a first key with **zero** known keys). crapto1 provides the solver
  pieces (`lfsr_common_prefix`); add the collection loop using
  `iso14443_3a_poller_txrx_custom_parity` (raw auth + controlled parity), guarded to
  `furi_hal_nfc_supports_raw_crypto1()`.
- New poller phase(s) or a dedicated helper under `applications/main/nfc/helpers/` +
  a `Darkside` entry in the enhanced-attack scene. Output: one recovered key → hands off to
  the Phase-1 nested pivot to get the rest.

### 5.4 Phase 3 — Hardnested
- Finish the collection buffering the poller already TODOs (`mf_classic_poller.c:9`
  "Buffer writes for Hardnested"): accumulate `nt_enc` + parity across many nested auths.
- Port the hardnested **offline solver** (bitflip-table state elimination; e.g. proxmark3
  `hardnested`), vendored VERBATIM with license into `applications/main/nfc/lib/hardnested/`.
  Tables + working set live in **PSRAM** (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`); it is
  memory- and CPU-heavy (minutes at 240 MHz) — acceptable, gated behind an explicit
  "Hardnested (slow)" confirm. Only reached when PRNG-type detection returns Hard.
- Recovered keys feed the same read→clone loop.

### 5.5 Clone loop (spans phases, delivered in Phase 1)
Recovered key set → full sector read (existing) → write dump incl. block 0/UID to a magic
card. Gen2 = solid direct path; Gen1a = backdoor path (28k). UID-block edit reuses the
`nfc_magic` gen pollers. A card whose keys are fully recovered becomes a working clone.

## 6. Files touched (by phase)

**Phase 1**
- Modify `components/furi_hal/furi_hal_nfc.h` + `furi_hal_nfc.c` — add
  `furi_hal_nfc_supports_raw_crypto1()` (ST25R3916 true / PN532 false).
- Modify `components/nfc/protocols/mf_classic/mf_classic_poller.c` — gate the partial-read
  bail on `!supports_raw_crypto1()`; enter the nested FSM on ST25R3916. Named timing/calib
  constants.
- Possibly modify `components/furi_hal/furi_hal_nfc.c` (custom-parity iso14443a path) +
  `st25r3916/st25r3916_hal.c` — only if nested nonce RX needs FDT/timing tightening.
- Modify `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c` /
  `views/dict_attack.c` — surface Enhanced (nested) progress + the "Write to magic" action.
- Modify/add `applications/main/nfc/scenes/nfc_scene_write.c` (or a new clone scene) —
  magic block-0/UID write via the `nfc_magic` gen pollers.

**Phase 2** — add `applications/main/nfc/helpers/mf_classic_darkside.{c,h}`; poller phase or
helper hook; scene entry.

**Phase 3** — add `applications/main/nfc/lib/hardnested/` (vendored solver, license header);
finish poller hardnested buffering; PSRAM allocation; "Hardnested (slow)" scene gate.

## 7. Verification split

- **What I verify:** compiles for target (both public + private variants, sdkconfig
  regenerated), boots clean, ST25R3916 detects, **no regression on a normal MIFARE read**,
  and — where possible — the recovery *math* against known nonce vectors in a host/target
  unit check (crapto1 is deterministic given nonces).
- **What only the tester verifies (I can't drive cards):**
  - P1: weak-PRNG access card (dict finds ≥1 key → nested recovers the rest → full read →
    clone to Gen2 and Gen1a → re-read clone matches). Static-nonce Fudan clone (static
    nested path).
  - P2: a card with **no** dictionary hit (darkside recovers a first key → nested pivot).
  - P3: a hard-PRNG card (EV1 / Plus-SL1) → hardnested.
- Each phase **ships only after the tester confirms** its card class. Build → deploy public
  webflasher per the established safe pipeline (regen sdkconfig, verify storage @0x390000).

## 8. Non-goals
- No crypto card-**emulation** / `mfkey32`-by-emulation (I2C timing wall).
- No PN532 nested/darkside/hardnested (kept on the safe partial-read fallback — no raw
  Crypto1). PN532 users still get dictionary + full read.
- No new card *families* here (DESFire/FeliCa/etc. are the separate "coverage" track).
- No 125 kHz LF (hardware is 13.56 MHz only).

## 9. Open items (resolve during Phase 1, on-device with the tester)
- Does weak-PRNG `Calibrate` produce stable `d_min`/`d_max` on the I2C-polled ST25R3916, or
  is FDT tightening needed? (Static nested is the fallback either way.)
- Is `nfc_scene_write.c` already magic-aware (block 0), or must the clone route through the
  `nfc_magic` pollers?
- Confirm the nested key solve runs **on-device** in the Enhanced path (not an external
  `.fap` offload like the legacy mfkey complete scene).
