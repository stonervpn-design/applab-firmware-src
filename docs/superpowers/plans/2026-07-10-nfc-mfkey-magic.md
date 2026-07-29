# NFC mfkey recovery + Magic-write Implementation Plan (HANDOFF)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. This is a FRESH-SESSION handoff — read the spec `docs/superpowers/specs/2026-07-10-nfc-mfkey-magic-design.md` first. Steps use checkbox (`- [ ]`).

**Goal:** On-device mfkey32 key recovery (vendor crapto1) replacing the external `mfkey.fap`, then magic-card writing (Gen2 solid, Gen1a best-effort). Crypto-correctness-critical — test with a known vector before any UI.

**Architecture:** Vendor crapto1 (`lfsr_recovery32`) into `applications/main/nfc/lib/crapto1/`; add an `mfkey32` recovery driver using it + the port's existing `crypto1_*` primitives + the `mfkey32_logger` param sets; replace the external-FAP branch in `nfc_scene_mf_classic_mfkey_complete.c` with on-device recovery → user dictionary. Phase 2: a "Write to Magic" flow from a saved dump using `mf_classic_poller` write_block (Gen2) and `iso14443_3a_poller_txrx_custom_parity` (Gen1a).

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper NFC stack (PN532). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Model policy:** implementers/reviewers on opus/haiku only (Sonnet-5 trips a cyber-safety gate on this offensive-security firmware). Crypto tasks: opus.
- **Correctness gate for crypto:** the mfkey recovery MUST pass a known test vector (see Task 2) before it is wired to any UI or trusted. A subtly-wrong `lfsr_recovery32` compiles fine and silently recovers nothing.
- **Do not regress** the existing Detect-Reader nonce collection or the normal NFC read/write/emulate flows. All changes additive except replacing the mfkey_complete external-FAP branch.
- **Symbol collision:** the port already has `components/nfc/helpers/crypto1.c` with `crypto1_*` symbols. The vendored crapto1 brings its own `crypto1_word`/`lfsr_*`. Namespace the vendored library (prefix `crapto1_` or keep it in its own TU with `static` linkage where possible) to avoid duplicate-symbol link errors. Reconcile at build time.
- Repo is git on `master`; commit per task. Leave untracked helper scripts alone.

---

### Task 1: Read + record the exact nonce param format

**Files:** (read-only investigation, then a short notes commit)
- Read: `applications/main/nfc/helpers/mfkey32_logger.c` — the internal param-set struct: exact fields per auth (`cuid`, `nt`, `nr_enc`, `ar_enc`, sector, key_type) and how `add_nonce` extracts them from `MfClassicAuthContext`. Read `MfClassicAuthContext` in `components/nfc/protocols/mf_classic/`.
- Read: `nfc_scene_mf_classic_detect_reader.c` + `views/detect_reader.c` — when/how `mfkey32_logger_add_nonce` is called and the "param set complete" (two-auth) condition.

- [ ] **Step 1:** Document (in the task report) the precise input the recovery needs: for each recoverable param set, the tuple `(uid, nt0, nr0_enc, ar0_enc, nt1, nr1_enc, ar1_enc, sector, key_type)`, and how to iterate the logger's collected sets (add a getter to `mfkey32_logger` if one isn't exposed — e.g. `mfkey32_logger_get_param(instance, i, Mfkey32Param* out)`).
- [ ] **Step 2:** If a getter is needed, add it to `mfkey32_logger.{c,h}` (non-breaking). Build.
- [ ] **Step 3:** Commit `feat(nfc): expose mfkey32 logger param sets for on-device recovery`.

---

### Task 2: Vendor crapto1 + mfkey32 recovery core + KNOWN-VECTOR test (no UI)

**Files:**
- Create: `applications/main/nfc/lib/crapto1/crapto1.c`, `crapto1.h` (+ `crypto1.c/.h` if the recovery variant ships its own) — vendored.
- Create: `applications/main/nfc/helpers/mfkey32_recover.c`, `.h` — the driver.

- [ ] **Step 1: Vendor crapto1.** From a canonical source (RfidResearchGroup/proxmark3 `common/crapto1/crapto1.c`+`crypto1.c`, or a Flipper mfkey app's bundled crapto1). Copy VERBATIM (keep license header) into `lib/crapto1/`. Namespace to avoid collision with the port's `components/nfc/helpers/crypto1.c` (see Global Constraints). Ensure it compiles standalone (pure C).
- [ ] **Step 2: mfkey32 driver.** `bool mfkey32_recover(const Mfkey32Param* p, uint64_t* out_key)`: implement the standard mfkey32 recovery using `lfsr_recovery32` + rollback + second-auth validation (port the exact driver from the same reference project — do NOT hand-roll the math). Return the 48-bit key on unique success.
- [ ] **Step 3: KNOWN-VECTOR TEST (the critical gate).** Add a small self-test (a debug scene, a unit-test entry, or a temporary `FURI_LOG` at NFC app init) that runs `mfkey32_recover` on a PUBLISHED mfkey32 test vector (uid + two nonces → known key, from the reference project's tests) and asserts the recovered key equals the expected key. **This must pass before proceeding.** Capture the pass in the task report. Leave the test in as a debug-gated check or remove after confirming (note which).
- [ ] **Step 4:** Build. Expect `=== BUILD OK ===`. The reviewer for this task MUST confirm the known-vector test passes (it's the correctness proof).
- [ ] **Step 5:** Commit `feat(nfc): vendor crapto1 + on-device mfkey32 recovery (known-vector verified)`.

---

### Task 3: Wire on-device recovery into the UI

**Files:**
- Modify: `applications/main/nfc/scenes/nfc_scene_mf_classic_mfkey_complete.c` (replace the external-FAP branch), possibly a new "recovering" progress scene/view.
- Reuse: the user-key-add path (`nfc_scene_mf_classic_keys_add` / the key dictionary write).

- [ ] **Step 1:** In `mfkey_complete` on_enter, instead of checking/running `NFC_MFKEY32_APP_PATH`, present a "Recover Keys on device" action. On select, iterate all logger param sets, call `mfkey32_recover` for each (show a progress/"Recovering…" view — recovery of many sets can take seconds; consider running in a worker thread to keep UI responsive, or a simple blocking popup for a first cut).
- [ ] **Step 2:** Collect recovered `(sector, key_type, key)` results; present them; offer "Save to user dictionary" → write via the existing key-add path. De-dupe against existing dict keys.
- [ ] **Step 3:** Keep "Save nonces to file" as a secondary option (backward-compat). Build.
- [ ] **Step 4:** Commit `feat(nfc): on-device Recover Keys action (no external mfkey.fap)`.
- [ ] **PAUSE for on-hardware flash-test** of mfkey before starting magic-write (per the brainstorm decision).

---

### Task 4: Magic-write Gen2 (write dump incl. block 0)

**Files:**
- New scene(s) under `applications/main/nfc/scenes/` (e.g. `nfc_scene_mf_classic_write_magic.c`) + a menu entry from a saved MIFARE Classic dump (the Saved-card info/actions menu).
- Reuse: `mf_classic_poller` write_block path.

- [ ] **Step 1:** From a saved MfClassic dump's actions, add "Write to Magic". Poller flow: for each sector, auth with the dump's key (A/B, fall back to FF), `write_block` every block INCLUDING block 0. (Gen2 accepts block-0 writes; the only delta from `nfc_scene_write.c` is not skipping block 0.)
- [ ] **Step 2:** Verify by re-reading block 0 (UID) + a couple data blocks; report success/failure per sector. Build.
- [ ] **Step 3:** Commit `feat(nfc): write saved dump to Gen2 magic card (incl. block 0/UID)`.

---

### Task 5: Magic-write Gen1a (best-effort, graceful)

**Files:**
- Extend the write-magic flow with a Gen1a path using `iso14443_3a_poller_txrx_custom_parity`.

- [ ] **Step 1:** Implement the Gen1a backdoor sequence via custom-parity txrx: `0x40` (7-bit short frame) → expect `0x0A`; `0x43` → ACK; then raw block-0 write (`0xA0` blockno → ACK → 16 data+CRC → ACK). **If the PN532 HAL cannot emit the 7-bit `0x40` short frame** (detect via error/timeout), abort cleanly and show "Gen1a not supported on this hardware" — never hang. Prefer Gen1a detection first; fall back to Gen2.
- [ ] **Step 2:** Build + (on-hardware) test against a Gen1a card if available; document whether the PN532 custom-parity path actually works. Commit `feat(nfc): Gen1a magic backdoor write (best-effort)`.

---

### Task 6: Docs

- [ ] `docs/NFC_MFKEY_MAGIC_TEST.md` (on-hardware checklist: Detect Reader → Recover Keys → key in dict → unlocks card; Write to Magic Gen2 → re-read matches; Gen1a attempt) + README note. Commit.

---

## Self-Review

**Spec coverage:** mfkey recovery (crapto1 + driver + vector test + UI) → Tasks 1-3; magic Gen2 → Task 4; Gen1a best-effort → Task 5; docs → Task 6. Nested/hardnested/Gen4 explicitly deferred. ✅
**Correctness gate:** Task 2 known-vector test is the hard gate before UI — prevents shipping a silently-broken recovery. ✅
**Risk register:** (1) crapto1 symbol collision with port crypto1 — namespaced (Global Constraints); (2) Gen1a PN532 custom-parity feasibility — Task 5 detects + degrades gracefully; (3) recovery latency — Task 3 worker/progress.
**Ordering:** 1→2→3 (pause/flash) →4→5→6.
