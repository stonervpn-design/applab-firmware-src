# B — Add land_rover_rke + toyota_keeloq decoders

> Subagent-driven-development. PRIVATE-only car material. Branch `feat/subghz-car-improve`.

**Goal:** add the 2 car protocols KAT/Flipper-ARF has that this tree genuinely lacks — both decode-only KeeLoq-hop RKE. Follow the firmware's decoder conventions exactly (template: `lib/subghz/protocols/land_rover_v0.c`).

## Global Constraints
- **PRIVATE-only.** Register both in `protocol_items.c` INSIDE the existing `#ifndef PUBLIC_BUILD` block (lines ~57-95, where land_rover_v0/toyota_v0 already live). Public build must not include them (public car-material exclusion). Verify: public m5stick `SENTINEL_OK` + `NOCAR PASS` + nm-scan 0 new car symbols.
- **Decode-only** (both): the 32-bit KeeLoq hopping code stays ENCRYPTED (no per-manufacturer key in firmware). Surface serial + button + raw hop; no decrypt, no crc-crypto-check. Encoder = decode-only stub (`encoder.upload = NULL` like `fiat_v0.c`, or the minimal encoder shape land_rover_v0 uses). Honest: `get_string` shows hop as encrypted/raw.
- **Structure:** mirror `land_rover_v0.c` — `SubGhzBlockConst`, `SubGhzProtocolDecoderX {base; generic; step; ...}`, `DecoderStepEnum`, `pp_is_short/pp_is_long` timing helpers, `const SubGhzProtocolDecoder ..._decoder`, `const SubGhzProtocol subghz_protocol_...` with `.type=SubGhzProtocolTypeDynamic`, `.flag = SubGhzProtocolFlag_433 | _315 | _AM/_FM | _Decodable`, get_hash_data/get_string per the base.
- Host-test each via the harness (`tools/decoder_host_test/`): synthesize a RAW duration stream for a known (serial,button,hop) frame, feed the decoder, assert extracted serial+button. WSL gcc.
- Fresh-log firmware build gate (private m5stick + cardputer green; public exclusion).

---

### Task 1: `land_rover_rke` decoder
**Files:** create `lib/subghz/protocols/land_rover_rke.{c,h}`; register in `protocol_items.c` (gated); host-test `tools/decoder_host_test/test_land_rover_rke.c`.

**Protocol (from KAT src/protocols/land_rover_rke.rs):**
- Modulation OOK/AM, fixed-width PWM, 1000us bit. Bit-1 = 700us HIGH + 300us LOW; Bit-0 = 300us HIGH + 700us LOW. Tolerance ±20%.
- Preamble: ≥16 (up to 20) pairs of (400us HIGH + 600us LOW). Sync: 400us HIGH + 9600us LOW (the long 9600us gap gates false matches — key discriminator). Inter-rep gap 12000us, 4 reps.
- Frame: 66 bits MSB-first. [65:34] = 32-bit KeeLoq hop (encrypted). [33:10] = 24-bit serial. [9:6] = 4-bit button (1=Lock,2=Unlock,4=Boot/Tailgate,8=Panic). [5:2] = 4-bit flags. [1:0] = 2-bit status (1=battery-low,2=repeat).
- Store 66 bits as data(u64 low 64) + extra(u64, bits 65:64). generic.data holds the low 64; keep the top 2 bits in a decoder field for serial/button extraction (serial/button are within the low 34 bits so generic.data suffices for those; hop spans the top — reconstruct from data+extra for display).
- crc_valid=false (no crypto check). Serial must be non-zero to avoid false positives.
- Freq flags: 433.92 + 315.

**Steps:** DecoderStep state machine: Reset → PreambleHigh/Low (count ≥16 400/600 pairs) → SyncLow (detect 9600us long low after a 400us high) → Data (collect 66 bits via PWM window: measure HIGH vs LOW ratio per 1000us bit → 1 if 700/300, 0 if 300/700). On 66 bits + serial!=0 → found; else reset. Extract serial=[33:10], button=[9:6].

- [ ] Impl `.h` (struct/const/decl), `.c` (state machine + get_string showing Serial/Button/Hop(raw)/flags), register gated, host-test (build a 66-bit frame's RAW durations for serial=0x123456 button=Unlock hop=0xDEADBEEF + preamble + 9600 sync → feed → assert serial/button). Build private m5stick green (fresh-log). Commit.

---

### Task 2: `toyota_keeloq` decoder (2 variants)
**Files:** create `lib/subghz/protocols/toyota_keeloq.{c,h}`; register gated; host-test `tools/decoder_host_test/test_toyota_keeloq.c`.

**Protocol (from KAT src/protocols/toyota.rs):** select variant by first HIGH pulse (>310us → A else B).
- Variant A (Corolla, 433.92): PWM pairs. LS (long HIGH+short LOW)=bit0, SL (short HIGH+long LOW)=bit1. te_short=400, te_long=800, delta=175. 68 bits (min 60). Preamble ≥6 short-short (SS) pairs; first non-SS pair begins data; gap terminates frame.
- Variant B (Tundra, 315): NRZ, midpoint classification per pulse. te_short=200, te_long=390, midpoint=287. 67 bits (min 60). Preamble short-HIGH/long-LOW pairs, terminated by sync-gap LOW 1500-2600us; or 67-bit count terminates.
- Frame data: `(hop<<32) | (serial<<4) | button` — hop 32-bit (encrypted), serial 28-bit, button 4-bit. No CRC; structural bit-count + serial!=0 is validity. Decode-only.
- Freq flags: 433.92 (A) + 315 (B).

- [ ] Impl `.h`/`.c` (variant-select state machine, both A PWM + B NRZ), get_string (Serial/Button/Hop raw/Variant), register gated, host-test both variants (synth A + B frames → assert serial/button), build private m5stick green. Commit. NOTE: distinct id from toyota_v0 (which is OOK-Manchester Estima 96-bit) — do NOT collide.

---

### Task 3: Verification + public exclusion + report
- [ ] Cardputer private build green (fresh-log). Public m5stick build `SENTINEL_OK` + `NOCAR PASS`; nm-scan public ELF for `land_rover_rke`/`toyota_keeloq` symbols → 0 (car material excluded).
- [ ] Both host-tests ALL PASS.
- [ ] Report `docs/superpowers/plans/subghz-car-B-report.md`: what shipped, both decoders decode-only (honest — hop encrypted), build results, 0-public-car-symbol proof, host-test results. HW-untested (no captures — needs real Land Rover/Toyota-KeeLoq fob to confirm on-air).
- [ ] Final review.

## Self-review notes
- Decode-only is correct + honest (no key). Both distinct from existing land_rover_v0 / toyota_v0.
- Gated in the car block; public excludes. Host-tested field extraction (not real-capture — HW-deferred).
