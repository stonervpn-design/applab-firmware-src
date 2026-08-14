# B — land_rover_rke + toyota_keeloq — verification report

**Date:** 2026-08-14
**Branch:** `feat/subghz-car-improve` (PRIVATE car material, unpushed).

## What shipped
The 2 car protocols KAT v1.3.0 / Flipper-ARF has that this tree genuinely lacked — both **decode-only** KeeLoq-hop RKE, ported from KAT's decoder logic, following `land_rover_v0.c` conventions.

| Decoder | Files | Commit(s) |
|---------|-------|-----------|
| `land_rover_rke` | `lib/subghz/protocols/land_rover_rke.{c,h}` + gated reg + host test | 3ee9749 |
| `toyota_keeloq` | `lib/subghz/protocols/toyota_keeloq.{c,h}` + gated reg + host test | 0d8f366, 0103e8d |

- **`land_rover_rke`**: OOK PWM, 66-bit — preamble 400/600 pairs + 400/9600 sync, bit 700/300(1) or 300/700(0). Extracts 24-bit serial + 4-bit button; 32-bit KeeLoq hop stays ENCRYPTED. Distinct from `land_rover_v0` (Diff-Manchester). Timing-disjoint (no cross-decode).
- **`toyota_keeloq`**: 2 variants — A (Corolla 433, PWM, 68-bit) / B (Tundra 315, NRZ, 67-bit), selected by first HIGH >310 µs. Extracts 28-bit serial + 4-bit button; 32-bit hop ENCRYPTED. Distinct from `toyota_v0` (OOK-Manchester Estima 96-bit).
- **Both decode-only + honest:** hop surfaced raw/`ENC`, `crc_check=false` ("NOT CHECKED, no key"), encoder = NULL stub. No fabricated decrypt/valid.

## Gating / public exclusion (PRIVATE car material)
Both registered in `protocol_items.c` inside `#ifndef PUBLIC_BUILD`. Proof:
- Public m5stick build `SENTINEL_OK` + `SENTINEL_VERIFY_NOCAR → PASS`.
- nm on public `furi_esp32.elf` (26,531 syms; known subghz sym present): `land_rover_rke`/`toyota_keeloq` = **0** in ELF and MAP.
- Sanity: both `*_protocol` symbols **present** in the private cardputer ELF.

## Build gate — fresh-log verified
| Target | Result |
|--------|--------|
| Private m5stick S3 | `SENTINEL_OK` (both decoder objs compiled) |
| Private Cardputer ADV | `SENTINEL_OK` |
| Public m5stick S3 | `SENTINEL_OK` + `NOCAR PASS` (0 new car symbols) |

## Host tests — ALL PASS (WSL gcc)
- `test_land_rover_rke`: PASS — serial=0x123456/button=Unlock/hop extracted from a synthetic 66-bit PWM frame.
- `test_toyota_keeloq`: PASS both variants (A + B) — serial=0x1234567/button/hop extracted.

## Review outcomes
- **land_rover_rke:** Spec ✅ + Approved. Reviewer hand-traced 66-bit carry-shift extraction (serial/button/hop math correct), verified no collision with land_rover_v0, gating + decode-only honesty. 2 Minors deferred (inherited template quirks, identical to land_rover_v0).
- **toyota_keeloq:** Variant A solid; Variant B Changes-requested (2 Important) → **FIXED**. The boundary was shape-inferred (could emit a plausible-but-WRONG decode = false-accept); now ends the preamble on the documented sync-gap LOW (1500-2600 µs) + requires ≥6 preamble pairs → **fail-safe** (declines ambiguous bursts). Bit-storage/extraction verified correct.

## Honest limits (HW-deferred / diagnostic)
- **`land_rover_rke`: NOT validated against a real signal** — the capture corpus has NO Land Rover capture. Host-tested on a synthetic frame only. Needs a real Land Rover fob to confirm on-air.
- **`toyota_keeloq`: the corpus's real Toyota captures (Corolla/Tacoma) do NOT decode with it** — histograms show they're a *different* Toyota protocol (likely `toyota_v0` Manchester or a rolling code), NOT KAT's NA-KeeLoq. This decoder targets NA-KeeLoq Toyota models absent from the corpus. Reported honestly (not faked). Synthetic-tested only.
- Both are **decode-only** (no key → hop can't be decrypted, so no emulate/clone).

## Net effect on decode coverage
The full-corpus harness run is unchanged at 37/174 by these 2 (neither protocol's real captures are in the corpus). Their value is *coverage* for real Land Rover / NA-KeeLoq-Toyota fobs a user might capture — not a fix to the existing corpus. The corpus decode gaps are the **A** work (separate).
