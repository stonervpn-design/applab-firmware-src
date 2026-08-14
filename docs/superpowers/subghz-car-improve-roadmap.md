# Sub-GHz Car Decoder Improvements — roadmap

**Goal (user, 2026-08-14):** improve the Sub-GHz car decoding. Investigated KAT v1.3.0 as a lead → **KAT is downstream of this firmware** (it sources from ProtoPirate = this tree). Firmware already has ~50 car decoders + 100+ Sub-GHz protocols vs KAT's 39. So the real improvements are (A) close this tree's own decode gaps, (B) add the ~2 genuine protocol variants KAT/Flipper-ARF has that we lack, (C) add emulate/TX to decode-only car protocols.

**PRIVATE-only.** All car material stays private, off public build + webflasher (owner password `lemon420` never given). Gate per the existing car-material convention. Branch `feat/subghz-car-improve` (off the RAM/red-team HEAD 152ef52).

## Three fronts
### B — add the 2 genuine new protocols (DOING FIRST; concrete, no captures needed)
KAT's only real gaps vs this tree (both decode-only KeeLoq, ported from KAT's Rust which mirrors Flipper-ARF):
- **`land_rover_rke`**: OOK PWM, 66-bit. te 700/300 (bit1) & 300/700 (bit0), 1000us bit. Preamble 16-20×(400H/600L); sync 400H/9600L; 12ms inter-rep, 4 reps. Frame MSB-first: [65:34] 32-bit KeeLoq hop, [33:10] 24-bit serial, [9:6] 4-bit button (1=Lock,2=Unlock,4=Boot,8=Panic), [5:2] flags, [1:0] status. Decode-only (hop stays encrypted; no key). 433.92/315 MHz. DISTINCT from `land_rover_v0` (Diff-Manchester).
- **`toyota_keeloq`** (2 variants, select by first HIGH >310us): A Corolla/433.92 PWM te 400/800 delta175, 68-bit, preamble ≥6 SS pairs, gap-terminated; B Tundra/315 NRZ te 200/390 mid287, 67-bit, sync-gap 1500-2600us. Frame: (hop<<32)|(serial<<4)|button — 32-bit hop, 28-bit serial, 4-bit button. Serial!=0 gate. Decode-only. DISTINCT from `toyota_v0` (OOK-Manchester Estima 96-bit).

### C — add encoders to decode-only car protocols (DOING; selective)
Decode-only car protocols found: `bmw_cas4` (fixed-code → replay works, encoder POINTLESS, skip), `fiat_v0`, `fiat_v1`, `kia_v1`, `audi_v3` (dev), + the 2 new B ones (KeeLoq hop, can't synth without key → decode-only by nature). Valuable C = wire encoders for the FIXED/known-cipher ones where we can synthesize valid frames (emulate/clone). Scope per-protocol: only add where we have the field->frame logic (round-trippable). Rolling ones needing an unknown cipher = leave decode-only (honest).

### A — close decode gaps (BLOCKED on captures)
Host harness (tools/decoder_host_test/pp_decode) found ~37/174 decode. Known failing: Kia V2/V3/V4/V6, Mazda, Renault, real-car Honda. **NEEDS the failing .sub captures** — not reachable here (on the cardputer SD). User to copy failing car .sub → Downloads. Meanwhile can analyze decoder logic blind (low-confidence).

## Method
Per protocol: port/impl → register in protocol_items.c (gated) → host-test (RAW timing round-trip / field extraction via tools/decoder_host_test) → fresh-log firmware build (private m5stick+cardputer green, public exclusion 0 car syms) → review. Same subagent-driven loop as the red-team program.
