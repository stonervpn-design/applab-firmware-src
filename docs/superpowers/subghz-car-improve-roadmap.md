# Sub-GHz Car Decoder Improvements — roadmap

**Goal (user, 2026-08-14):** improve the Sub-GHz car decoding. Investigated KAT v1.3.0 as a lead → **KAT is downstream of this firmware** (it sources from ProtoPirate = this tree). Firmware already has ~50 car decoders + 100+ Sub-GHz protocols vs KAT's 39. So the real improvements are (A) close this tree's own decode gaps, (B) add the ~2 genuine protocol variants KAT/Flipper-ARF has that we lack, (C) add emulate/TX to decode-only car protocols.

**PRIVATE-only.** All car material stays private, off public build + webflasher (owner password `lemon420` never given). Gate per the existing car-material convention. Branch `feat/subghz-car-improve` (off the RAM/red-team HEAD 152ef52).

## Three fronts
### B — DONE 2026-08-14 (5 commits, report subghz-car-B-report.md)
✅ land_rover_rke + toyota_keeloq shipped: gated/private, decode-only, host-tested, per-task reviewed (toyota Variant-B false-accept -> fail-safe sync-gap fix), public 0 syms. HONEST: land_rover_rke has NO real capture (synthetic only); toyota corpus captures are a DIFFERENT protocol (disclosed, not faked). Neither changes the 37/174 (their real targets aren't in the corpus). Full-corpus harness re-run = still 37/174.

### B — add the 2 genuine new protocols (original spec below)
KAT's only real gaps vs this tree (both decode-only KeeLoq, ported from KAT's Rust which mirrors Flipper-ARF):
- **`land_rover_rke`**: OOK PWM, 66-bit. te 700/300 (bit1) & 300/700 (bit0), 1000us bit. Preamble 16-20×(400H/600L); sync 400H/9600L; 12ms inter-rep, 4 reps. Frame MSB-first: [65:34] 32-bit KeeLoq hop, [33:10] 24-bit serial, [9:6] 4-bit button (1=Lock,2=Unlock,4=Boot,8=Panic), [5:2] flags, [1:0] status. Decode-only (hop stays encrypted; no key). 433.92/315 MHz. DISTINCT from `land_rover_v0` (Diff-Manchester).
- **`toyota_keeloq`** (2 variants, select by first HIGH >310us): A Corolla/433.92 PWM te 400/800 delta175, 68-bit, preamble ≥6 SS pairs, gap-terminated; B Tundra/315 NRZ te 200/390 mid287, 67-bit, sync-gap 1500-2600us. Frame: (hop<<32)|(serial<<4)|button — 32-bit hop, 28-bit serial, 4-bit button. Serial!=0 gate. Decode-only. DISTINCT from `toyota_v0` (OOK-Manchester Estima 96-bit).

### C — add encoders to decode-only car protocols (DOING; selective)
Decode-only car protocols found: `bmw_cas4` (fixed-code → replay works, encoder POINTLESS, skip), `fiat_v0`, `fiat_v1`, `kia_v1`, `audi_v3` (dev), + the 2 new B ones (KeeLoq hop, can't synth without key → decode-only by nature). Valuable C = wire encoders for the FIXED/known-cipher ones where we can synthesize valid frames (emulate/clone). Scope per-protocol: only add where we have the field->frame logic (round-trippable). Rolling ones needing an unknown cipher = leave decode-only (honest).

### A — REFRAMED 2026-08-14 (the "fix failing decoders" premise is WRONG)
Per the 2-day-old harness analysis ([[protopirate-host-decode-harness]]): **NO decoder-logic bugs.** The 137 NODECODE are (1) DIFFERENT protocol variants the decoders correctly REJECT (Mazda RX8 timings 133/263/795/1743 != mazda_v0 250/500; Renault Megane2005 != renault_v0; etc.) -> closing them = NEW decoders (coverage, like B); (2) KEY-GATED KeeLoq (Kia V3/V4/V5/V6 need mfg keys; with keystore loaded -> 41 not 37, my run didn't load keys = the 4-cap gap); (3) noisy captures. So real A = (a) add coverage decoders for the highest-value unimplemented variants (data-driven from clean NODECODE captures), + (b) ensure on-device key provisioning for the KeeLoq makes. NOT bug-fixing. Requires: triage clean-vs-noise per make, identify the unimplemented protocol per clean capture, port it (B-style). PRESENT this reframing to user before committing to the (larger-than-expected) coverage work.

### A — close decode gaps (ORIGINAL framing below, superseded by the reframing above)
**CORPUS: `C:\Users\stone\Downloads\auto-subghz-kakuzu\` = exactly 174 car .sub (matches the "37/174" harness figure — THE test set).** Organized American/Asian/European → make folders. Counts: Hyundai_Kia_Genesis 33, VW_Audi 15, Groupe_PSA 14, Nissan 13, Lexus_Toyota 13, GM 11, Honda_Acura 11, Mazda 9, Suzuki 7, Ford 6, Mercedes_BMW 6, FIAT 4, Chrysler_Jeep 4, Renault 3, +Range Rover 1, etc. Read via `wsl.exe -e bash -lc` (plain /mnt/c is flaky in the Bash tool; explicit wsl.exe works). Also: Downloads\auto_subghz_test (174, likely dup), community_car_subghz (209), FlipperZero-Subghz-DB-main (13717 full DB). Playlists index (make->file names): Downloads\Automotive-Sub-Ghz-Playlists-main.zip (KAKUZU). Method: point tools/decoder_host_test/pp_decode at the corpus, batch-decode, list pass/fail per make, diagnose failing decoders (Kia V2/V3/V4/V6, Mazda, Renault, real Honda), fix, re-run.

## Method
Per protocol: port/impl → register in protocol_items.c (gated) → host-test (RAW timing round-trip / field extraction via tools/decoder_host_test) → fresh-log firmware build (private m5stick+cardputer green, public exclusion 0 car syms) → review. Same subagent-driven loop as the red-team program.
