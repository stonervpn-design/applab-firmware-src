# Signal-ID fingerprint engine (prototype)

The safe core of the "What is this?" flagship (see
`docs/superpowers/specs/2026-07-31-signal-id-engine-design.md`). Turns a Sub-GHz
capture's characteristics into a ranked, explained identification + suggested
action. **Pure logic — never transmits, never touches a radio.**

Deliberately kept OUT of the firmware source glob (`lib/subghz/*.c`) so the
shipping build is untouched until reviewed. Move `signal_fingerprint.{c,h}` +
`signal_fingerprint_db.inc` into `lib/subghz/` (and wire a UI scene) to ship it.

## Files
- `build_fingerprints.py` — mines fingerprints from `lib/subghz/protocols/*.c`
  decoder constants (ground truth), runs the reference classifier + self-tests,
  and emits the C table. This is the *verified* implementation.
- `signal_fingerprint.{h,c}` — the C engine, a line-for-line mirror of the Python.
- `signal_fingerprint_db.inc` — auto-generated `static const` table (47 automotive
  fingerprints). Regenerate; do not hand-edit.
- `fingerprints.json` — same data for the future cloud-DB spoke.

## Run
```
python tools/signal_id/build_fingerprints.py            # mine + self-tests
python tools/signal_id/build_fingerprints.py --demo     # + example outputs
python tools/signal_id/build_fingerprints.py --emit-c   # regenerate C table + json
```

## Verification status
- Python reference: **self-tests pass** (self-match, ±8% perturbation, off-band →
  UNKNOWN, decoded-name authoritative, genuine same-timing collisions → ambiguous).
  Real execution on Python 3.14.
- C engine: **compiles clean** for esp32s3 (`xtensa-esp32s3-elf-gcc -Wall -Wextra
  -Werror`). Not executed — this box has no native C host runner, only the cross
  compiler; the C is a direct mirror of the Python that the vectors validated.

## Key finding
~12 automotive protocols are **signature-identical** (e.g. 250/500 µs, 64-bit, AM,
433 MHz is shared by Ford V0 / MazdaSiemens / Star Line / Revers). Timing+bits+band
alone cannot separate them — the engine correctly returns them as `ambiguous`. The
authoritative path (a decoder already matched → the name is ground truth) is what
delivers certainty; fingerprinting adds make/model + action to a decode, and gives
a family shortlist for still-undecoded/raw captures.

## Not built (by design — needs your review / hardware)
- UI scene wiring in the SubGhz app.
- The action spokes: RollBack/attack, Guardian/defend loop, cloud DB, AI copilot.
  Anything that transmits or attacks was intentionally NOT written blind.
