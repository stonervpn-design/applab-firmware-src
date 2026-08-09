# AUT64 Key Recovery — Design (2026-08-09)

PRIVATE-only, authorized research. Offline HOST tool: recover the 64-bit AUT64 key
(VAG immobilizer/RKE cipher) from captured frames, exploiting AUT64's known
structural weaknesses. Design/spec only.

## Goal

Given known plaintext↔ciphertext pairs from a VAG AUT64 fob, recover the 8-byte
key (and its key-dependent s-box/p-box permutations) so the existing VAG
encoder/emulate path can generate valid frames.

## What exists

- `lib/subghz/protocols/aut64.{c,h}`: verified AUT64 — 12 rounds, 8-byte block,
  8-byte key, key-derived s-box (16) + p-box (8) + 256-entry offset table.
- `vag.c` / [[fzcard-vag-keyunknown-not-a-bug]]: VAG AUT64 decode + encoder +
  keystore; on-air capture stalls at 69/80 bits (front-end SNR ceiling).

## HARD CONSTRAINTS / honest feasibility (read first)

1. **Known-plaintext is required.** The AUT64 cryptanalysis (Garcia-Oswald et
   al., "Lock It and Still Lose It", 2016) recovers the key + permutations from a
   few dozen known plaintext↔ciphertext pairs. You must know the PLAINTEXT
   (counter/serial being encrypted) for each captured ciphertext. Whether the VAG
   RKE plaintext is fully known/derivable per frame is the crux — Phase 0 decides.
2. **Capture quality is already the bottleneck.** VAG stalls at 69/80 bits on
   this bare front-end ([[fzcard-vag-keyunknown-not-a-bug]]); a cryptanalytic
   attack needs COMPLETE, correct frames. Partial captures poison the pairs. This
   is a hardware ceiling no host tool fixes.
3. **Offline only**; heavy but tractable IF clean known-plaintext pairs exist.

## Approach (phased, synthetic-first)

- **Phase 0 — plaintext model + breakability (the gate).** Confirm the VAG AUT64
  frame's plaintext structure (what the counter/serial layout is, and whether
  it's known per capture). In Python, drive the real AUT64 cipher: generate pairs
  under a known key, prototype the structural recovery (exploit the key-dependent
  permutation weakness), measure pairs needed. Green only if the plaintext is
  knowable AND recovery is feasible.
- **Phase 1 — recovery core (host).** Implement the permutation + key recovery,
  validated on synthetic pairs (known key → recover exactly). Rust/C for speed.
- **Phase 2 — real-capture pipeline.** Ingest clean VAG captures (reject <80-bit
  partials), form plaintext↔ciphertext pairs, recover the key, verify it
  re-encrypts every captured frame; write to the private keystore.

## Verification

Synthetic round-trip: known key → generate pairs via the verified aut64.c →
recover key + permutations → assert equality + re-encryption. Real captures only
after that passes AND the front-end delivers full 80-bit frames.

## Open questions for the owner

1. Is the VAG AUT64 frame plaintext (counter/serial) known/derivable per capture,
   or opaque? (No known-plaintext → no attack.)
2. Can the front-end deliver clean 80-bit VAG frames, or is the 69/80 SNR ceiling
   still blocking (in which case this is hardware-gated before any crypto)?
