# Doorbell Brute — PT2262 tri-state address brute (2026-08-10)

PRIVATE-only, authorized-testing. Turn the Doorbell Brute app from a 38-code
known-database *replay* into a real **brute** that actually rings PT2262/PT2260/
SC2262 DIP-switch wireless doorbells, plus keep the known-code pass for the rest.

## Goal

Ring as many wireless doorbells as feasible by sweeping the tri-state address
space of Princeton-family fixed-code bells. The old "2^24 = days" estimate was
wrong: a PT2262 code is 12 tri-state symbols (each pin = 0 / 1 / float), so the
address space is 3^N, not 2^24. Sweeping the 8 address symbols = 3^8 = 6561
combinations — minutes, not days.

## Scope

- **In scope:** PT2262 / PT2260 / SC2262 DIP-switch fixed-code bells (address set
  by tri-state pins). These are brute-able because the address is a small 3^8
  space and the receiver rings whenever the transmitted address matches its DIP.
- **Out of scope for the brute:** EV1527 / learning-code bells (random 20-bit
  factory address, must be paired) — not brute-able. The existing 38-code known
  DB pass still covers the known ones of these.

## Verified encoding (the correctness core — do NOT guess this)

Princeton PWM (Flipper `princeton.c` encoder): each of 24 bits →
bit 1 = high `3·te` / low `te`; bit 0 = high `te` / low `3·te`; then a
`guard·te` (~30·te) low sync gap. The RX self-clocks `te` from the waveform.

PT2262 tri-state symbol → 2 Princeton bits (MSB-first):
`0 → 00`, `1 → 11`, `float → 01` (short-then-long).

VERIFIED against the real DB (see `tools`/analysis): 19/36 codes are clean
tri-state (all pairs ∈ {00,11,01}); the intertechno family `0x??5455` decodes to
address symbols ∈ {0,float} with data symbols = `01 01 01 01` (0x55). The other
17 codes contain `10` pairs → EV1527/other, out of scope.

Word layout (24-bit, MSB first): **8 address symbols = top 16 bits**, **4 data
symbols = low 8 bits**. Brute index `n ∈ [0, 3^8)` → base-3 digits → 8 address
symbols → top 16 bits; data byte fixed per pass.

Data default: **0x55** (all-float) rings 17/19 clean DB bells; sweep set
`{0x55, 0xFF, 0x00}` to also catch all-1 / all-0 receivers.

## Sweep structure

Ordered so the most-likely bells ring first; user stops when their bell chimes.

- **Phase 0 — known DB (fast, ~20s):** replay the 38 baked codes (framed
  Princeton) exactly as today. Rings known bells (incl. non-tri-state) immediately.
- **Phase 1 — tri-state brute:** for `freq` in {selected} × `te` in {250,400} ×
  `data` in {0x55,0xFF,0x00} × `n` in [0,6561): build the 24-bit word, TX framed
  Princeton, `repeat=2`, tight cadence. Full run per freq = 3·2·6561 ≈ 39k frames.

Frame ≈ 96·te + guard ≈ ~40ms @ te=350; `repeat 2` + short reset gap ≈ ~120ms/
candidate → ~13 min per (data,te) block, ~1.3h full per freq (but rings on hit,
so effective time to a given bell is far less).

## TX path

Reuse the existing framed-Princeton encoder (`subbrute_worker` manual transmit =
the real `.sub`-replay waveform — PWM symbols + sync gap, the same path that
already rings on replay). Feed it generated 24-bit words. Drop the current 300ms
`DB_INTER_CODE_MS` to a tight brute gap (~30-50ms) — at 6561 codes the old gap
alone is ~33 min of dead time.

## Config / UX

Keep the app shell + sweep thread. Config (VariableItemList): **Frequency**
(433.92 / 315 / both), **Repeats** (per-candidate frame repeats, default 2),
**Start**. Status view: phase label, `addr n/6561`, `te`, `data`, `freq`,
progress bar within the current block, Back = stop. Notifications on hit-block
boundaries as today.

## Files

- `doorbell_codes.h` — unchanged (Phase 0 table).
- `doorbell_tristate.h` — NEW: `db_tristate_word(uint32_t n, uint8_t data)` →
  24-bit Princeton word from base-3 address index + data byte; the te/data/freq
  sets.
- `doorbell_brute.c` — rework the sweep thread into Phase 0 (DB) + Phase 1
  (tri-state), tight TX cadence, updated status/config.
- Mirror all into `src_cardputer_adv`. Private-only, both trees. Build both.

## Verification

- Host round-trip (already done): confirm `db_tristate_word` for known intertechno
  codes reproduces the exact DB key (e.g. n encoding {0,0,0,0,f,f,f,0} + data 0x55
  == 0x005455). Assert a couple in a tiny host test / boot self-check.
- On-device: RF-test against a real PT2262 bell (the true acceptance test the
  de Bruijn version failed). Encoding is the proven framed-Princeton replay path,
  so a matching (address,data,te) MUST ring.
