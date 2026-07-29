# MIFARE Classic crack → clone (ST25R3916) — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On the ST25R3916 backend, turn the already-present-but-gated nested attack on so an unknown MIFARE Classic card's keys are recovered on-device (weak-PRNG + static nested), then add a "Write to Magic Card" action that clones the recovered dump (incl. block 0/UID) onto a Gen1a/Gen2 magic card.

**Architecture:** The nested FSM already exists in `mf_classic_poller.c` and auto-engages during the normal Read flow (`enhanced_dict` is set true at the user-dict phase). It bails to a partial (dict-only) read because it was written for the PN532, which cannot send raw Crypto1. Add a HAL capability query `furi_hal_nfc_supports_raw_crypto1()` (true on ST25R3916, false on PN532) and gate the bail on it — the FSM then runs the nested attack on the raw-capable ST25R3916 while PN532 keeps its safe fallback. Then wire a clone action through the existing `nfc_magic` Gen1a/Gen2 pollers.

**Tech Stack:** C, ESP-IDF 5.4.1, ESP32-S3 (8 MB PSRAM), ST25R3916 over I2C, crapto1 (vendored).

## Global Constraints

- Board `m5stick_s3`; ST25R3916 backend is the target (`NfcBackendSt25r3916`). PN532 MUST keep its current dictionary-only + partial-read behavior — **no regression** on PN532.
- The change must NOT alter any byte-aligned read/write/auth that works today; the nested path only runs where it previously bailed (raw-capable backend), so normal reads are unaffected — that is the de-risk.
- Source tree is NOT a git repo. "Checkpoint" = run the stated verification and confirm; no `git commit`.
- Build private: `powershell -c "& '<abs>\build_only_private.bat'"`; public: `build_only_public.bat`. Confirm `SENTINEL_OK` in the matching `.log`, `0` `error:` lines, and a FRESH `build_m5stick_s3\furi_esp32.bin` mtime (Git-Bash `cmd /c` mangles the path and silently no-ops — always invoke the `.bat` via PowerShell and verify the bin mtime advanced).
- Flash app-only: `python -m esptool --chip esp32s3 -p COM10 -b 460800 --before default_reset --after hard_reset write_flash 0x10000 build_m5stick_s3\furi_esp32.bin`.
- Crypto correctness is safety-critical: do NOT modify crapto1 / crypto1 / the nested solver math. Phase 1 only *enables* and *routes*; it does not touch the recovery arithmetic.
- Card-driving verification is the tester's (I cannot drive NFC cards). Each on-device crypto behavior is a tester checkpoint.

---

## File Structure
- **Modify** `components/furi_hal/furi_hal_nfc.h` — declare `furi_hal_nfc_supports_raw_crypto1()`.
- **Modify** `components/furi_hal/furi_hal_nfc.c` — implement it (ST25R3916 true / PN532 false), mirroring the existing backend-branch functions.
- **Modify** `components/nfc/protocols/mf_classic/mf_classic_poller.c` — gate the "PN532 cannot do nested → partial" bail on `!furi_hal_nfc_supports_raw_crypto1()`.
- **Modify** `applications/main/nfc/scenes/nfc_scene_mf_classic_read_success.c` (or the read-success submenu scene) + `applications/main/nfc/nfc_app_i.h` — add a "Write to Magic Card" action.
- **Add** `applications/main/nfc/helpers/mf_classic_magic_clone.{c,h}` — thin wrapper that drives the `nfc_magic` Gen1a/Gen2 pollers with a `MfClassicData` dump.

---

## Task 1: HAL capability query `furi_hal_nfc_supports_raw_crypto1()`

**Files:**
- Modify: `components/furi_hal/furi_hal_nfc.h`
- Modify: `components/furi_hal/furi_hal_nfc.c`

**Interfaces:**
- Produces: `bool furi_hal_nfc_supports_raw_crypto1(void)` — true iff the active backend can transmit raw Crypto1 (encrypted-parity, no-CRC) frames, i.e. the ST25R3916 custom-parity path. False on PN532 and when no HAL is ready.

- [ ] **Step 1: Declare the query in the header**

In `components/furi_hal/furi_hal_nfc.h`, next to the other capability/bool queries (near `furi_hal_nfc_is_hal_ready` ~line 126), add:

```c
/** True iff the active NFC backend can transmit RAW Crypto1 frames (encrypted
 * parity, no CRC) — i.e. the ST25R3916 custom-parity path. Required for the
 * nested/darkside key-recovery attacks. False on PN532 (InDataExchange cannot
 * emit raw Crypto1) and when no frontend is up. */
bool furi_hal_nfc_supports_raw_crypto1(void);
```

- [ ] **Step 2: Implement it, mirroring the backend-branch pattern**

In `components/furi_hal/furi_hal_nfc.c`, next to `furi_hal_nfc_low_power_mode_start` (which shows the `if(nfc_backend == NfcBackendSt25r3916)` pattern), add:

```c
bool furi_hal_nfc_supports_raw_crypto1(void) {
    if(!nfc_hal_ready) return false;
    /* ST25R3916 sends raw Crypto1 via st25r3916_hal_transceive_custom_parity
     * (the same path MIFARE read/write already uses). PN532's InDataExchange
     * abstracts frames and cannot, so nested/darkside are ST25R3916-only. */
    return (nfc_backend == NfcBackendSt25r3916);
}
```

- [ ] **Step 3: Build (private) and verify it compiles**

Run (PowerShell): `& 'C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_only_private.bat'`
Expected: `SENTINEL_OK`, no `error:` lines, `build_m5stick_s3\furi_esp32.bin` mtime advances.

- [ ] **Step 4: Checkpoint** — confirm `SENTINEL_OK` and fresh bin. (No git.)

---

## Task 2: Enable nested on the raw-capable backend (gate the bail)

> **REVISED 2026-07-28 (code reading + partial on-device evidence):** The original
> "flip the bail" instruction is WRONG and unsafe. Code reading shows the NoTag bail
> only fires when `mf_classic_poller_get_nt()` FAILS, and on the ST25R3916 `get_nt`
> uses the standard/custom-parity paths that already work — so when the card is present
> the bail is skipped and **the nested FSM already proceeds with no change**; flipping the
> bail would only risk the documented infinite loop on a genuine card-loss. On-device we
> confirmed the enhanced flow completes a full Classic read with NO hang (32/32, stable).
> The remaining unknown — does `get_nt` succeed so nested *engages* — is decided by
> watching the live `dict_attack` view for the "Nested Dictionary"/"Collect NtEnc" phase
> headers during a NON-default card read (a default-keyed card finishes at the dictionary
> and never enters nested). **Status: PENDING that on-device observation.** If nested
> engages → Task 2 is a no-op (no code change). If `get_nt` fails on ST25R3916 → the fix
> is a targeted repair of the collect path, NOT a bail flip. Do NOT change the bail.

**Files:**
- Modify: `components/nfc/protocols/mf_classic/mf_classic_poller.c`

**Interfaces:**
- Consumes: `furi_hal_nfc_supports_raw_crypto1()` (Task 1).

- [ ] **Step 1: Confirm the current bail**

Open `mf_classic_poller.c`, function `mf_classic_poller_handler_nested_analyze_prng` region (~lines 1929–1950). The `MfClassicNestedPhaseAnalyzePRNG` branch checks `prng_type == MfClassicPrngTypeNoTag` and, on the PN532, sets `instance->state = MfClassicPollerStateSuccess` (partial read) because `nested_collect_nt` "always fails (Crypto1 cannot be sent raw)". Note the exact `if(dict_attack_ctx->prng_type == MfClassicPrngTypeNoTag) { … return command; }` block.

- [ ] **Step 2: Ensure the header is included**

At the top of `mf_classic_poller.c`, confirm `#include <furi_hal_nfc.h>` is present (the poller already calls furi_hal_nfc indirectly via the iso14443_3a poller; add the include if the symbol is not visible at compile time).

- [ ] **Step 3: Gate the partial-read bail on backend capability**

Change the `NoTag` bail so it only short-circuits when the backend CANNOT do raw Crypto1. When it can (ST25R3916), a genuine `NoTag` here means the card actually left the field (a real error), so fall through to the existing controller path rather than force partial success. Replace:

```c
        if(dict_attack_ctx->prng_type == MfClassicPrngTypeNoTag) {
            FURI_LOG_E(TAG, "No tag detected");
```
with:
```c
        if(dict_attack_ctx->prng_type == MfClassicPrngTypeNoTag &&
           !furi_hal_nfc_supports_raw_crypto1()) {
            /* PN532 (no raw Crypto1): nested is impossible → emit dict keys found
             * so far as a PARTIAL success. On a raw-capable backend (ST25R3916)
             * we do NOT take this bail — the nested FSM runs for real. */
            FURI_LOG_I(TAG, "Nested unsupported on this backend; partial read");
```

(Leave the body of the block — the nonce-array free + `instance->state = MfClassicPollerStateSuccess; return command;` — unchanged. Only the guard condition changes.)

- [ ] **Step 4: Build (private)**

Run the private build (PowerShell). Expected: `SENTINEL_OK`, no `error:`, fresh bin.

- [ ] **Step 5: Flash + on-device engagement check (I can verify this part)**

Flash to COM10. Capture serial while reading a normal MIFARE Classic card (any card): confirm the log now shows the nested FSM advancing (`AnalyzePRNG` / `Calibrate` / `CollectNtEnc` trace lines) instead of the immediate "partial read", AND that a plain fully-dictionaried card still reads to success (no regression). Command:

```
python -c "import serial,time; s=serial.Serial('COM10',115200,timeout=1); s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False); t=time.time();
[print(l.decode('utf-8','replace').strip()) for _ in iter(int,1) if (l:=s.readline()) and time.time()-t<20]"
```

- [ ] **Step 6: Checkpoint** — `SENTINEL_OK`; on-device log shows nested engaging on ST25R3916 with no regression on a normal read.

---

## Task 3: On-device crack verification + timing tuning (tester)

**Files:**
- Possibly modify: `components/nfc/protocols/mf_classic/mf_classic_poller.c` (or `mf_classic_poller_i.h`) — ONLY the named nested calibration/FDT constants, if timing needs tuning.

**Interfaces:** none new — this task proves the recovery on real cards and exposes one tuning knob.

- [ ] **Step 1: Static-nested card (timing-insensitive — should work first)**

Tester: read a static-nonce / backdoor MIFARE Classic (e.g. a Fudan clone) where the dictionary finds ≥1 key. Expected: the attack recovers the remaining sector keys and the read completes with all sectors green. This path has no `Calibrate` dependency, so it validates the raw-frame routing independent of timing.

- [ ] **Step 2: Weak-PRNG card (the timing unknown)**

Tester: read a genuine weak-PRNG MIFARE Classic 1K with ≥1 dict key. Expected: `AnalyzePRNG` → `Calibrate` → nested recovers the rest. If keys come back **wrong or empty** while static-nested worked, it is the `Calibrate` nonce-distance timing (jitter on the I2C path), NOT the crypto.

- [ ] **Step 3: If Step 2 is unstable — tighten the auth turnaround**

Only if needed: issue the nested auth at a repeatable delay using the existing block-tx FDT timer (`furi_hal_nfc_timer_block_tx_start_us`) around the nested auth in `mf_classic_poller_handler_nested_calibrate` / `..._collect_nt_enc`, and/or raise `MF_CLASSIC_NESTED_CALIBRATION_COUNT`. Expose both as named constants at the top of the file so tuning is one line. Rebuild + re-flash; tester re-checks. Do NOT touch the solver math.

- [ ] **Step 4: Checkpoint** — tester confirms an unknown card (≥1 dict key) is fully keyed and read on ST25R3916. Record which card classes pass (static / weak-PRNG). A weak-PRNG failure that can't be tuned is acceptable to defer — static + dict + (Phase 3) hardnested still cover it — but must be logged honestly, not hidden.

---

## Task 4: "Write to Magic Card" clone action

**Files:**
- Add: `applications/main/nfc/helpers/mf_classic_magic_clone.{c,h}`
- Modify: `applications/main/nfc/scenes/nfc_scene_mf_classic_read_success.c` (read-success submenu)
- Modify: `applications/main/nfc/nfc_app_i.h` (custom event enum if a new one is needed)

**Interfaces:**
- Produces: `bool mf_classic_magic_clone_write(Nfc* nfc, const MfClassicData* src, MfMagicType type, FuriString* status)` — writes `src` (all blocks incl. block 0/UID) onto a magic card of `type` (Gen1a or Gen2), returns success and a human-readable status.

- [ ] **Step 1: Read the nfc_magic write path to get the exact poller API**

Open `applications_user/nfc_magic/` — the scene that writes a saved dump to a magic card (its write/poller scene) and `magic/protocols/gen1a` + `gen2` poller headers. Note the exact call to program block 0/UID and blocks (the Gen1a backdoor `0x40`/`0x43` sequence; the Gen2 direct block-0 write). This is the code `mf_classic_magic_clone_write` will mirror.

- [ ] **Step 2: Implement the clone wrapper**

Create `mf_classic_magic_clone.c/.h` that, given a `MfClassicData*`, drives the Gen1a or Gen2 poller found in Step 1 to write every block including block 0. Reuse the nfc_magic poller verbatim — do not reimplement magic framing. Guard Gen1a behind `furi_hal_nfc_supports_raw_crypto1()` (its backdoor needs the raw 7-bit `0x40` frame — the same path fixed in 28k); Gen2 works on any backend.

- [ ] **Step 3: Add the submenu action**

In the MF Classic read-success scene, add a "Write to Magic Card" submenu item (visible when the loaded data is `NfcProtocolMfClassic`). Selecting it prompts Gen1a/Gen2, presents the standard "Apply the card to the back" popup, calls `mf_classic_magic_clone_write`, and shows the returned status.

- [ ] **Step 4: Build (private)** — PowerShell build; `SENTINEL_OK`, no `error:`, fresh bin.

- [ ] **Step 5: Flash + tester clone check**

Tester: after cracking+reading an unknown card (Task 3), use "Write to Magic Card" → Gen2 magic → re-read the magic → UID + all blocks match the source. Repeat for Gen1a. This closes the loop.

- [ ] **Step 6: Checkpoint** — tester confirms a cracked card clones to Gen2 (and Gen1a) and the clone re-reads identical.

---

## Task 5: Ship Phase 1 (both variants + webflasher)

**Files:** none (build/deploy only).

- [ ] **Step 1: Build both variants with regenerated sdkconfig**

PowerShell: run `build_only_private.bat` then `build_only_public.bat`. Each MUST delete+regenerate `sdkconfig` from the m5stick defaults (the batches already do). Confirm `SENTINEL_OK` and fresh bins for both.

- [ ] **Step 2: Deploy the public webflasher (safe pipeline)**

Run `deploy_webflasher_public.bat`; confirm it stages `webflash\Flipper-m5stick_s3-public-merged.bin` and prints `OK: 'storage' partition present at 0x390000`. Run `python tools\verify_partition_table.py <merged.bin>` — must pass.

- [ ] **Step 3: Bump manifest + push**

Bump `webflash\manifest.json` `"version"` to `public-2026-07-28<next>`. In `webflash\`: `git add -A && git commit && git push origin main`.

- [ ] **Step 4: Update memory**

Update `[[fzcard-st25r3916-nfc-backend]]`: nested/enhanced key recovery is enabled on the ST25R3916 backend (gated by `furi_hal_nfc_supports_raw_crypto1()`), with which card classes are tester-verified, plus the Gen2/Gen1a clone action. Note Phases 2 (darkside) and 3 (hardnested) remain.

- [ ] **Step 5: Checkpoint** — Phase 1 shipped; report to user what the tester verified and that Phase 2 (darkside) is next.

---

## Notes for the implementer
- The crack side is almost entirely Tasks 1–2 (a capability query + a one-condition guard). Everything else is verification (Task 3) and the clone UI (Task 4). Resist adding scope.
- Do NOT touch crapto1 / crypto1 / the nested solver arithmetic. Phase 1 enables and routes; it never changes recovery math.
- PN532 must be provably unchanged: its `supports_raw_crypto1()` returns false, so it takes the exact same partial-read bail as today.
- If weak-PRNG timing can't be stabilized on I2C, ship static-nested + dict now and revisit under Phase 3 — but log it plainly; never present a partial capability as complete.
