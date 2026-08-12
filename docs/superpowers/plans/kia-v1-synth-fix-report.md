# Kia V1 multi-button emulate — synthesize fix report

Status: **FIXED — Kia V1 now truly synthesizes** (encode -> decode round-trips serial+button+counter).
Path taken: focused encoder fix (polarity + framing + count), **not** a rewrite and **not** a reclassify.

## Symptom
Host sweep (`tools/decoder_host_test/fob_sweep`) before the fix:

```
Kia V1  Close(0x1)  FAIL  decoded Btn:D5  (wanted 0x1)
Kia V1  Open(0x2)   FAIL  decoded Btn:EA  (wanted 0x2)
Kia V1  Boot(0x3)   FAIL  decoded Btn:F5  (wanted 0x3)
# Kia V1 COUNTER: CNT-PARTIAL  [N=AAA N+1=555 N+2=AAA]  (alternating -> gross mis-encode)
```

The decoder was known-good (it decodes real captures). The encoder
(`lib/subghz/protocols/kia_v1.c`, `kia_protocol_encoder_v1_get_upload`) emitted a
frame the decoder could not reconstruct.

## Root cause (three compounding bugs in the encoder's bit emission)
The Kia V1 decoder uses the stateful Flipper `manchester_advance` transition table
(`components/toolbox/manchester_decoder.c`) fed by `pp_manchester_event`. I dumped a
real capture (`Kia_V1_N1_RAW.sub`) through a faithful replica of `feed()` and read out
the exact per-symbol decode. The real wire format is a **state-dependent (differential)
Manchester**, where consecutive equal half-bits merge into a single `te_long` pulse:

| Manchester state | bit | emitted symbols                | next state |
|------------------|-----|--------------------------------|------------|
| Mid1 (line high) |  1  | LOW `te_short`, HIGH `te_short`| Mid1       |
| Mid1 (line high) |  0  | LOW `te_long`                  | Mid0       |
| Mid0 (line low)  |  1  | HIGH `te_long`                 | Mid1       |
| Mid0 (line low)  |  0  | HIGH `te_short`, LOW `te_short`| Mid0       |

The old encoder instead emitted every bit as two fixed `te_short` half-pulses with
**inverted** polarity (`1 = HIGH,LOW`) and **never merged** equal adjacent half-bits.
That is not a valid Manchester stream for this decoder, so it desynced immediately.

Two further framing details the old code got wrong:

1. **Missing leading anchor.** After the start-bit separator the decoder's Manchester
   state is `Mid1`; the real signal carries one absorbed HIGH `te_short` (Mid1->Mid1, no
   bit) that re-establishes phase before the first data bit. Without it the first real
   half-bit is eaten.
2. **One bit too many.** `subghz_protocol_blocks_add_bit` both shifts in the start bit
   *and increments* `decode_count_bit`, so the decoder counts the start bit as 2 and then
   consumes only `min_count_bit_for_found - 2 = 55` Manchester symbols. The most-
   significant payload bit is already carried by the start separator, so the loop must
   emit **55** data bits starting at `data_count_bit-1`, not 56 starting at
   `data_count_bit`. Emitting 56 shifted the entire decoded payload right by one bit —
   which is exactly the intermediate `btn 0x2 -> 0x01`, `cnt 0x3E8 -> 0x1F4` symptom seen
   mid-fix.

## The fix (encoder only; decoder untouched)
In `kia_protocol_encoder_v1_get_upload`:
- keep the `te_long` header and the LOW `te_short` start separator unchanged;
- add one HIGH `te_short` **anchor** pulse;
- replace the data loop with the differential Manchester state machine above, iterating
  bits `data_count_bit-1 .. 1` (MSB first), tracking Mid1/Mid0 and emitting the matching
  short/long pulses;
- bump `KIA_V1_UPLOAD_CAPACITY` by one pulse/burst for the anchor (still far under the
  shared slab).

The decoder, timing constants, preamble, `te_short/te_long`, burst count and inter-burst
gap are all unchanged, so real captures still decode identically.

## Verification
Host generator/decoder replica (matches the real `feed()` exactly, validated against
`Kia_V1_N1_RAW.sub`: 9 frames, btn 0x03, cnt 0x122): **20000/20000** random
serial/button/counter round-trips PASS.

Full `fob_sweep` after the fix — Kia V1 rows:

```
Kia V1  Close(0x1)  PASS  Btn:01:Close
Kia V1  Open(0x2)   PASS  Btn:02:Open
Kia V1  Boot(0x3)   PASS  Btn:03:Boot
# Kia V1 COUNTER: CNT-ADVANCES(synth)  [N=3E8 N+1=3E9 N+2=3EA]
CAP Kia V1 Close/Open/Boot PASS   # capture-seeded (firmware-accurate) path
# CAP Kia V1 COUNTER: ADVANCES(synth) (runs=3 moves=2)
```

Counter round-trips exactly (1000/1001/1002 -> 0x3E8/0x3E9/0x3EA).

### No regressions
`diff` of the full sweep (before vs after) shows the **only** changed lines are the Kia V1
synthetic + capture-seeded rows flipping to PASS and the summary totals:

- SUMMARY(synthetic):      PASS 25 -> 28  (+3 = the three Kia V1 buttons)
- SUMMARY(capture-seeded): PASS 13 -> 16  (+3)

Every other protocol row is byte-identical.

## Firmware build
Private Cardputer-ADV build via `build_adv_only_private.bat` reached **SENTINEL_OK**
(SENTINEL_START -> BUILD -> MERGE -> OK). `furi_esp32.bin` = 0x2e3980 bytes, app partition
52% free; `Flipper-cardputer_adv-merged.bin` regenerated. (The background wrapper reported
exit 1, but the log's terminal marker is SENTINEL_OK and both bins are fresh — the non-zero
exit is a spurious esptool/`endlocal` residual, not a build failure.)

## Concerns / notes
- Decoded `serial` carries a forced high bit (start bit lands in serial bit31 because the
  decoder's payload is 56 bits with the start at bit55). This is inherent to the decoder,
  affects only serial bit31, and does **not** affect button or counter. For real captured
  fobs the serial already round-trips (the bit was set when the frame was first decoded);
  for synthetic 24-bit test serials the low 24 bits still match. Button + counter — the
  fields the emulate feature and the grid depend on — round-trip exactly.
- Not RF-tested on a real Kia/Hyundai receiver; verification is host round-trip + real-
  capture structural match only. On-air confirmation remains pending like the other fob
  protocols.
