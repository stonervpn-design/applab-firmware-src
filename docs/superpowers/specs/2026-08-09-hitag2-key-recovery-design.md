# Hitag-2 Key Recovery — Design (2026-08-09)

PRIVATE-only, authorized research. Offline HOST tool to recover the 48-bit Hitag-2
key from captured RKE frames — the "PC/GPU job" the on-device forge tool notes as
out of scope. Design/spec only; no code yet.

## Goal

Given N captured Hitag-2 RKE frames from one fob (same UID), recover the 48-bit
on-air key so the existing forge/clone path can then generate valid future codes
offline. Runs on the PC (Python/Rust), NOT the ESP32.

## What already exists

- `lib/subghz/protocols/hitag2_cipher.{c,h}`: verified 48-bit LFSR + f20 filter
  (byte-exact vs MIKRON), `hitag2_init(key, uid, iv)`, `hitag2_ks32`, rev32/rev48
  bit-order helpers.
- `tools/hitag2_make_forge.py`: parses a captured .sub (UID / button / low-10
  counter / captured Ks), and — given a KNOWN key — brute-forces the 18 unknown
  high counter bits to reproduce Ks (proves key + bit-order). This is the
  verifier the recovery tool reuses.
- [[hitag2-rke-forge]]: mask 0xCE0044C101CD, LSB-first bit reversal, RKE (not the
  LF interactive auth).

## HARD CONSTRAINTS / honest feasibility (read first)

1. **This is RKE mode, not the LF interactive auth.** The famous Hitag-2
   key-recovery results ("Gone in 360 Seconds", Verdult-Garcia-Balasch) attack
   the reader↔transponder AUTHENTICATION (LF, 2-way), which the ESP32 cannot
   capture (no LF coil). Here the input is the **autonomous UHF RKE keystream**
   (32 bits leaked per frame), a DIFFERENT setting whose breakability with a
   feasible frame count + compute is a genuine **research question**, not a
   settled result. Phase 0 must answer it before promising a working attack.
2. **Cost is unknown until measured.** Depending on how much of the 48-bit state
   each 32-bit `ks32` constrains and how the nonce/IV is derived from the counter,
   recovery ranges from tractable (guess-and-determine over a few frames) to
   infeasible-without-GPU. Report the honest estimate from Phase 0; do NOT ship a
   tool that claims a break it can't deliver.
3. **Offline only** — heavy search; the ESP32's role is purely capturing frames
   and exporting them.

## Approach (phased, research-first)

- **Phase 0 — breakability study (synthetic, the gate).** In Python, drive the
  real cipher: pick a key, generate M frames (UID + counter → nonce → ks32).
  Measure how many `ks32` samples pin the 48-bit state, and prototype a
  guess-and-determine / algebraic recovery (fix the LFSR feedback + filter,
  solve). Output: "recoverable from ~K frames in ~T on this hardware" or "not
  feasible offline without GPU/more leakage." This decides whether Phases 1-2 are
  worth building. HONEST result reported either way.
- **Phase 1 — recovery core (if Phase 0 is green).** Implement the recovery in a
  fast language (Rust/C, mirroring `kat_decode`), validated against synthetic
  frames (known key → recover it exactly). Reuse `hitag2_make_forge.py`'s counter
  brute + Ks check as the correctness oracle.
- **Phase 2 — real-capture pipeline.** Ingest N real captured .subs (same UID),
  parse UID/counter/Ks, run recovery, print the 48-bit key + verify it reproduces
  every captured Ks. Then hand off to the existing forge to generate future codes.
- **Phase 3 — keystore write-back.** Optionally load the recovered key into the
  private keystore so the firmware forge/clone uses it directly.

## Verification

Synthetic round-trip is the spine: generate frames with a known key via the
verified cipher, recover the key, assert equality + Ks reproduction. Only after
that passes on synthetic data do real captures mean anything. No claim of a break
without the synthetic recovery demonstrably working.

## Open questions for the owner

1. How many frames can you realistically capture from one fob (tens? hundreds?) —
   sets the feasible attack budget.
2. Acceptable that Phase 0 may conclude "not offline-feasible for RKE mode" — in
   which case the honest deliverable is the negative result + the synthetic
   study, not a working break?
3. Do you also have LF-auth captures (e.g. from a Proxmark) — which would enable
   the classic, proven attack instead of the harder RKE-only path?
