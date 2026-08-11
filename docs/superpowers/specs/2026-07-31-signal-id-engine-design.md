# Signal Identification Engine ("What Is This?") — Design

**Date:** 2026-07-31
**Status:** Draft for review (authored autonomously overnight; nothing on this
branch transmits or attacks — pure classification logic only. See "Safety".)
**Target tree:** `lib/subghz/` (the private `build_m5stick_s3.bat` firmware), not
the standalone `applications_user/proto_pirate` app.

## The one-liner

> The device that identifies **anything wireless around you** and tells you
> **what you can do about it.**

Point it at a captured Sub-GHz signal (later: BLE / Wi-Fi / NFC) and it names the
make/model/protocol family, says whether it's fixed or rolling, and surfaces the
relevant **action** — attack it (red-team), defend against it (Guardian), or
explain it (AI copilot). The identification engine is the **hub**; attack,
defend, and explain are **spokes**.

Why this is the product and not a pile of apps: the firmware already contains ~30
automotive decoders, Defender, FindMy, the Tesla scanner, the applab cloud-build
service, and a local pentest RAG. None of them answer the first question a user
actually has — *"what is this thing?"* — and none compound. An identification hub
turns those scattered capabilities into one story, and (with the cloud DB spoke)
into a **moat**: every capture makes identification better, and a competitor
cannot copy the accumulated capture corpus in a weekend the way they can copy a
single attack.

## Scope of THIS spec (v1, the safe core)

v1 is the **Sub-GHz fingerprint engine**, pure logic, no radio control, no UI:

- Input: a capture's measurable characteristics — frequency (Hz), modulation
  (AM/OOK vs FM/FSK), decoded bit count, and timing (`te_short`, `te_long` in µs);
  optionally the decoded protocol name if a decoder already matched.
- Output: a **ranked identification** — for each candidate: protocol name,
  make/model, family, fixed-vs-rolling, TX-capable-on-this-firmware, a confidence
  score, and a suggested-action tag (`attack` / `defend` / `capture` / `explain`).
- The classifier is a scoring function over a **fingerprint database** that is
  **auto-mined from the decoders themselves** (`te_short`/`te_long`/bit-count/
  modulation/band come straight from each decoder's `SubGhzBlockConst` and its
  protocol flags), so the DB cannot silently drift from the firmware.

Explicitly OUT of v1 (each is its own later spec):
- Any transmit / attack execution (RollBack, replay, brute) — **spoke, gated on
  hardware testing + your review**.
- Guardian real-time detection loop (jammer/deauth/tracker) — spoke.
- Cloud DB upload/match and AI copilot — spokes.
- UI / SubGhz-app scene wiring — needs your UX input.

## Architecture

```
                 capture characteristics
                          │
                          ▼
        ┌─────────────────────────────────────┐
        │  signal_fingerprint (pure C module)  │
        │  - fingerprint DB (mined table)      │
        │  - score(candidate, observed)        │
        │  - classify() -> ranked results      │
        └─────────────────────────────────────┘
                          │ ranked ID + action tags
          ┌───────────────┼────────────────┐
          ▼               ▼                 ▼
     attack spoke     defend spoke      explain spoke
     (RollBack…)      (Guardian…)       (RAG copilot…)
        [later]          [later]           [later]
```

- **`signal_fingerprint.h/.c`** — one module, one job: turn observed
  characteristics into a ranked, explained identification. No radio, no storage,
  no UI. Depends only on the mined DB table (a `static const` array). Testable in
  isolation: feed characteristics in, assert the ranking out.
- **`gen_fingerprint_db.py`** — build-time miner: parses `lib/subghz/protocols/
  *.c` for each protocol's `SubGhzBlockConst` (te_short/te_long/min bits) + the
  protocol `.flag` line (bands + AM/FM) + the name macro + encoder presence, and
  emits `signal_fingerprint_db.inc` (the `static const` table) plus a JSON copy
  for the cloud spoke. Ground-truth, regenerable, no hand-typed timings.
- **`signal_fingerprint_ref.py`** — a runnable reference implementation of the
  exact scoring logic, with test vectors, so the algorithm is *verified by
  execution* even though the Windows box has no native C host compiler (only the
  xtensa cross-compiler). The C mirrors this file rule-for-rule.

## Scoring (v1)

For an observed `(freq, mod, bits, te_short, te_long)` and a DB candidate:

1. **Hard filters** (mismatch → candidate dropped): modulation class (AM vs FM)
   must match; observed frequency must fall in one of the candidate's bands
   (±band tolerance).
2. **Soft score** (0..100), weighted:
   - bit-count closeness (exact = full marks, ± a few bits = partial),
   - `te_short` ratio closeness, `te_long` ratio closeness (ratio, not absolute,
     so a preset/clock offset doesn't tank the match),
   - a small bonus when the decoded protocol name is already known and matches.
3. Return candidates sorted by score, each tagged: `rolling?`, `tx_capable?`,
   `make/model`, and `suggested_action` (rolling+tx → `attack`; known-jammer/
   tracker family → `defend`; unknown/undecoded → `capture`; anything → `explain`).

Confidence = top score, with a "low confidence / unknown — raw capture only"
result when nothing clears a floor.

## Error handling

- Empty/again-zero timings or bits → return the single `UNKNOWN` result with a
  `capture` action; never crash, never fabricate a make.
- Ties within a small score delta → return all tied, mark `ambiguous` so the UI
  (later) can ask the user or the cloud can disambiguate.
- The engine is **advisory**: it never selects a radio, never transmits. A wrong
  ID costs a mislabel, not an RF action.

## Testing

- `signal_fingerprint_ref.py` runs a vector suite: (a) every DB protocol's own
  mined signature must identify as itself at high confidence; (b) perturbed
  signatures (±10% timing, ±2 bits) still rank the right family top-1; (c) a
  deliberately off-band / wrong-modulation signature returns `UNKNOWN`, not a
  false positive. This runs natively (Python) = real verification.
- The C module is compiled with the xtensa toolchain to prove it builds into the
  firmware; its logic is a line-for-line mirror of the Python that the vectors
  validated. (No native C host runner on this machine — stated honestly.)

## Safety / ethics

Everything in v1 is **classification only**. It reads characteristics and returns
labels. It does not, and cannot, transmit, jam, unlock, or brute anything. The
attack spokes (RollBack et al.) are deliberately deferred: they act on real
vehicles/devices, must only run in authorized testing, and must be verified on
hardware with you present before they ship. The identification hub is safe to
build and demo anywhere.

## Open questions for your review

1. Buyer/positioning still open (you punted on 1/2/3) — this hub deliberately
   serves all three, but the first *spoke* we build should match your buyer.
2. Where should the ID surface live — a new "Analyze" scene in the SubGhz app, or
   its own app? (UI is out of v1; flagging for when we wire it.)
3. Cloud DB: opt-in upload only, anonymized — confirm the privacy posture before
   the cloud spoke.
