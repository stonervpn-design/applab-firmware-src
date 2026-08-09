# KeeLoq Secure-Learning Clone — Design (2026-08-09)

PRIVATE-only, authorized research. Extend the KeeLoq no-desync clone
([[keeloq-no-desync-clone]]) to the SECURE-LEARNING manufacturers the rolling
analyzer currently excludes (BFT / Erreka / FAAC-class), whose device key is
derived from a per-fob SEED rather than from the serial alone. Design/spec only.

## Goal

Clone a secure-learning KeeLoq fob (currently gated out of clone/save) by
obtaining its seed, deriving the device key with the known manufacturer key, and
feeding that key into the existing no-desync clone TX path.

## What already exists (crypto is done)

`lib/subghz/protocols/keeloq_common.c` already implements every device-key
derivation:
- `..._secure_learning(serial, seed, key)` — device key from serial + seed + mfr key.
- `..._faac_learning(seed, key)`, `..._learning_erreka(data, mix, key)`,
  `..._magic_xor_type1_learning`, `..._magic_serial_type{1,2,3}_learning`,
  `..._learning_aerf`, `..._learning_pujol`.
- The no-desync clone (kdev+serial+counter → synthesized N/N+1 frames) works once
  the device key is known — see [[keeloq-no-desync-clone]].

So the ONLY missing input is the **seed**. The rolling_analyzer `can_clone` gate
excludes BFT/Erreka precisely because "a normal button press never carries the
seed" (its own comment).

## The seed problem (the whole feature) — three manufacturer classes

1. **Seed-derivable from serial/data** (magic-XOR / magic-serial / some FAAC):
   the "seed" is a deterministic function of the captured serial/data. For these,
   NO extra capture is needed — un-exclude them and derive the key from the
   button press we already decode. Lowest-hanging fruit.
2. **Seed carried in a PROGRAMMING/PAIRING frame** (true secure learning — BFT,
   Erreka): the fob transmits the seed only during learning mode (hold both
   buttons / programming sequence). Cloning requires capturing THAT frame, not a
   normal press. The encoder already knows the BFT programming-mode seed format
   (keeloq.c "transmit its seed in hop part"); the decoder must recognize +
   extract it.
3. **Seed unobtainable without the fob's internals** (fully sealed): out of scope
   — say so in-UI.

## Architecture

- **Seed source resolver** `keeloq_seed_for(manufacturer, serial, data, captured_seed)`
  → returns the seed (or "need pairing capture"). Class-1 mfrs derive it; class-2
  return the captured seed if a pairing frame was seen, else signal the operator.
- **Pairing-frame capture (class 2)**: in the analyzer, a "Capture Pairing" mode
  that recognizes the learning/seed frame (distinct bit layout) and stores its
  seed alongside the fob. Pure decode — host-testable against known BFT vectors.
- **Device-key derivation**: dispatch to the existing `_secure_learning` /
  `_faac_learning` / `_learning_erreka` / magic-* by manufacturer (data present).
- **Clone**: hand (serial, device_key, counter) to the existing no-desync clone;
  un-exclude these mfrs in `can_clone` once a key is derivable.
- PRIVATE-only; the mfr keys already live in the private keystore (never public).

## Phased plan

- **Phase 1 — class-1 (derivable seed) clone.** Pure `keeloq_seed_for` +
  device-key dispatch for magic-XOR/serial + FAAC-derivable, host-tested against
  known keystore vectors (encode→derive→decode round-trip). Un-exclude those
  mfrs in `can_clone`. NO new capture UI. Delivers the most makes for least risk.
- **Phase 2 — class-2 pairing-seed capture.** Recognize + decode the BFT/Erreka
  learning/seed frame; store the seed with the fob; derive + clone. Host-test the
  frame parser against captured BFT programming vectors.
- **Phase 3 — analyzer UX + guardrail.** "Secure clone" surfaced for the
  un-excluded mfrs; "Capture Pairing" for class-2; nm-gate the whole thing out of
  public.

## Honest caveats

- Class-1 works from a normal press (real, immediate). Class-2 needs the operator
  to capture the fob's PAIRING transmission (a distinct action — the target must
  be put in learning mode), which is a practical, not a crypto, limitation.
- Manufacturer key must already be in the private keystore (it is for the covered
  mfrs); this does NOT recover unknown manufacturer keys — that's the separate
  slide-attack host tool.
- No HW test until on-radio against the owner's own secure-learning fob.

## Open questions for the owner

1. Phase-1 makes to prioritize (which magic-XOR/serial + FAAC mfrs you actually
   have fobs for)?
2. Do you have a class-2 (BFT/Erreka) fob you can put into pairing mode for the
   Phase-2 seed-frame capture, or keep Phase 2 host-vectors-only for now?
