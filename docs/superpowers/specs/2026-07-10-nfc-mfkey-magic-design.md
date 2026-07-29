# NFC: On-device mfkey recovery + Magic-card writing — Design Spec

**Date:** 2026-07-10
**Status:** Approved scope (brainstormed). **HANDOFF to a fresh session** — this is a
large, crypto-correctness-critical feature deliberately deferred from the long
build session that produced it, so the crypto gets built with full context.

## Goal (scoped, in order)

1. **On-device mfkey32 recovery** — turn the nonce pairs already collected by the
   "Detect Reader" flow into recovered MIFARE Classic keys **on the device**, added
   to the user key dictionary — with NO external `mfkey.fap` required.
2. **Magic-card writing** — write a saved MIFARE Classic dump (incl. block 0 / UID)
   to a magic card: **Gen2** (direct block-0 write) as the solid path; **Gen1a**
   (backdoor commands) as best-effort (PN532 custom-parity feasibility risk).

**Deferred (out of scope for now):** nested / hardnested / darkside (PN532-over-I2C
timing is unproven for these); Gen4/GTU magic.

## What already exists in the port (don't rebuild)

- **Nonce collection:** `applications/main/nfc/helpers/mfkey32_logger.{c,h}` —
  `mfkey32_logger_alloc(cuid)`, `mfkey32_logger_add_nonce(logger,
  MfClassicAuthContext*)`, `mfkey32_logger_get_params_num()`,
  `mfkey32_logger_save_params(path)`, `mfkey32_logger_get_params_data(FuriString)`.
  The Detect-Reader flow (`nfc_scene_mf_classic_detect_reader.c`) emulates a card,
  a reader authenticates, and each auth's nonce data is logged. Two auths on the
  same (sector,keytype) with the same key → one recoverable "param set".
- **Recovery is currently offloaded:** `nfc_scene_mf_classic_mfkey_complete.c`
  checks `NFC_MFKEY32_APP_PATH` (`/ext/apps/NFC/mfkey.fap`) and runs it externally.
  We are REPLACING that offload with on-device recovery.
- **crypto1 primitives present:** `components/nfc/helpers/crypto1.c` has
  `crypto1_filter`, `crypto1_prng_successor`, `lfsr_rollback_bit`,
  `crypto1_lfsr_rollback_word`, `crypto1_decrypt_nt_enc`,
  `crypto1_encrypt_reader_nonce`, `crypto1_nonce_matches_encrypted_parity_bits`,
  `crypto1_is_weak_prng_nonce`. **MISSING: `lfsr_recovery32` (the forward
  state-enumeration) — the core of mfkey.**
- **Magic-write building blocks:** `components/nfc/protocols/mf_classic/
  mf_classic_poller.h` exposes a poller with `key`, `write_block`,
  `write_block_provided`, and `MfClassicBackdoor` types (Auth1/2/3). The existing
  `nfc_scene_write.c` writes a dump to a **normal** card (same-protocol) via
  `nfc_protocol_support` — it does NOT do block-0/UID or magic backdoors.
- **Raw framing for Gen1a:** `components/nfc/protocols/iso14443_3a/
  iso14443_3a_poller.h` → `iso14443_3a_poller_txrx_custom_parity(instance, tx, rx,
  fwt)` — the custom-parity primitive Gen1a's 7-bit `0x40` backdoor + `0x43`
  needs. **Feasibility on PN532-over-I2C is the one hardware unknown.**

## The mfkey recovery gap: vendor crapto1

The missing piece is the **crapto1** library (by bla / nfc-tools), specifically
`lfsr_recovery32(uint32_t ks2, uint32_t in)` which enumerates the ~2^19 candidate
LFSR states consistent with an observed 32-bit keystream, plus `crypto1_word` /
`lfsr_rollback_word` helpers. This is public (GPL/BSD) and small (~300 lines).

**Sourcing (fresh session, first task):** vendor `crapto1.c` + `crapto1.h` (and its
`crypto1.c`/`crypto1.h` if the recovery variant needs its own filter tables) from a
canonical source — e.g. RfidResearchGroup/proxmark3 `common/crapto1/`, or the
crapto1 bundled in a Flipper mfkey app (github.com/noproto or bettse's mfkey).
Vendor VERBATIM with license header into
`applications/main/nfc/lib/crapto1/`. Reconcile symbol collisions with the port's
existing `crypto1.c` (rename the vendored ones under a `crapto1_` prefix or a
static translation unit if the port's `crypto1_*` clash).

## mfkey32 recovery algorithm (what to build on top of crapto1)

Standard mfkey32 (reader-attack, same key twice):
- Input per param set: `uid` (cuid), and two auths `{nt0, nr0_enc, ar0_enc}` /
  `{nt1, nr1_enc, ar1_enc}` (the fields `mfkey32_logger` stores — read its `.c` for
  the exact struct/order).
- Compute `ks2 = ar0_enc ^ prng_successor(nt0, 64)`; `lfsr_recovery32(ks2, 0)` →
  candidate states; for each, `lfsr_rollback_word` past nr0 and uid⊕nt0 to get the
  key state; validate the candidate against the SECOND auth (nt1/nr1/ar1) — the
  surviving unique state rolls back to the 48-bit key. (Use the exact reference
  mfkey32 driver from the same source as crapto1 — do not hand-roll the math.)
- On success, add the recovered key (sector, keytype) to the user dictionary
  (`nfc_scene_mf_classic_keys` / the key-add path already exists).

## UI change

Replace the "run external mfkey.fap" branch in
`nfc_scene_mf_classic_mfkey_complete.c` with an on-device **Recover Keys** action:
run recovery over all collected param sets (this can take seconds — show progress /
a "Recovering…" view), then present recovered keys and offer to save them to the
user dictionary. Keep the nonce save/export as a secondary option.

## Magic-card writing (phase 2)

- New flow from a **saved** MIFARE Classic dump (Saved → a dump → "Write to Magic").
- **Gen2:** authenticate each sector with the dump's keys (or FFs) and
  `write_block` all blocks INCLUDING block 0 (Gen2 accepts block-0 writes). Reuse
  the `mf_classic_poller` write_block path; the only delta from normal write is
  writing block 0. Verify by re-reading.
- **Gen1a (best-effort):** via `iso14443_3a_poller_txrx_custom_parity`: send the
  Gen1a magic wakeup `0x40` (7-bit) → expect `0x0A`; `0x43` → ACK; then raw write
  block 0 with the standard MIFARE write sequence (0xA0 blockno → ACK → 16 data +
  CRC → ACK). If the PN532 HAL can't do the 7-bit `0x40` short frame, detect the
  failure and show "Gen1a not supported on this hardware" — do NOT hang.
- Gate the feature: detect magic type first (try Gen1a backdoor; else assume Gen2
  if block-0 write succeeds); surface which worked.

## Testing

- **Compile gate:** `build_ci.bat`.
- **mfkey unit-ish check:** feed a KNOWN mfkey32 test vector (uid/nonces→key,
  from the reference project's test data) through the recovery and assert it
  returns the known key — this is the critical correctness gate (a subtly-wrong
  `lfsr_recovery32` builds fine but recovers nothing). Do this BEFORE wiring the UI.
- **On-hardware:** Detect Reader against a real reader → collect ≥1 param set →
  Recover Keys → key appears in the dictionary and unlocks the card. Magic: write a
  saved dump to a Gen2 magic card → re-read matches (incl. UID); try a Gen1a card.

## Attribution

crapto1 / mfkey32 vendored from its GPL/BSD source (keep the header). Integration,
UI, and magic-write flows are new.

## Handoff notes

- This spec + its plan (`docs/superpowers/plans/2026-07-10-nfc-mfkey-magic.md`) are
  self-contained. Start the fresh session by reading both.
- The rest of the session's work (8 features) is on `master`, MERGE-clean, in the
  flashable bin — flash-test those first if not already done.
- Progress ledger convention: `.superpowers/sdd/progress.md` (git-ignored scratch).
