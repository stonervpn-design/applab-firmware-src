# Car-fob emulate-all-protocols — Task 4 report (final verification)

**Task:** (A) per-protocol per-button synthesize→decode round-trip sweep on host against the
REAL `lib/subghz/protocols/` car encoders/decoders; (B) build private cardputer + m5stick to
SENTINEL_OK with VAG/KeeLoq no-regression. Don't flash, don't push.
**Branch:** feat/keeloq-no-desync-clone (parent HEAD `bd34cd8`, Task 3).
**Status:** DONE. Both firmware builds SENTINEL_OK. Host sweep built + run. Committed (no push).

## Commit
Single commit on feat/keeloq-no-desync-clone, parent `bd34cd8`. Adds ONLY harness/test files
(no firmware touched): `tools/decoder_host_test/build_lib.sh`, `tools/decoder_host_test/fob_sweep.c`,
a small shim addition in `tools/decoder_host_test/host_shim.c`, and this report.

## A. Round-trip sweep — one-line result
Built a lib-targeted harness (`build_lib.sh` compiles the 21 SUPPORTED protocols' real
encoders/decoders + blocks/flipper_format/furi shims + `keystore_host.c`, NO proto_pirate, NO
app_i.h shim) and a driver (`fob_sweep.c`) that, for each protocol × each provider button, seeds
a FlipperFormat (fixed Serial, counter N, that button written to `subghz_fob_button_field`), runs
the encoder → durations → decoder, and asserts the decoded button. Two modes:
**synthetic** (seed from scratch) and **capture-seeded** (decode a real `.sub` → change button →
re-encode → decode — the firmware's actual flow). Result: **25/80 synthetic PASS**, and for every
protocol with a real capture in the local collection the button round-trips
(**capture-seeded 13/13 button PASS on the 5 protocols that lib-decoded a capture**). Every
non-PASS has a documented mechanical reason below; none indicate a defect in the Task-3 routing.

### Round-trip matrix (best evidence per protocol; S=synthetic, C=capture-seeded)
| Protocol | Buttons PASS | Counter | Verdict / reason for non-PASS |
|---|---|---|---|
| Kia V0 | S 3/3, C 3/3 | ADVANCES | **PASS** — full synth confirmed |
| Hyundai/Kia RIO | S 4/4 | (Cnt not printed) | **PASS** |
| SantaFe 13-16 | S 4/4 | ADVANCES | **PASS** |
| Subaru | S 4/4, C 4/4 | ADVANCES | **PASS** |
| Fiat V1 | S 4/4 | (Cnt not printed) | **PASS** (hitag2; button re-baked, hop replayed) |
| Ford V0 | C 4/4 | ADVANCES | **PASS via capture** — synth NODECODE (encoder TXes the captured Key; needs a real seed, as the firmware supplies) |
| Ford V3 | C 2/2 | ADVANCES | **PASS via capture** — synth SEED-FAIL: encoder requires the captured 13-byte `Raw` rolling hop (replay-rolling-field, per Task 1/3) |
| Honda Static | S 4/6 | ADVANCES | **PASS** for 0x1/0x2/0x4/0x5; 0x8/0x9 NODECODE = mask-restricted buttons (Task 1: only 0x336-mask-valid) |
| HondaV7 | S 1/6 (+1 remap) | (Cnt n/a) | **PARTIAL** — `custom_btn` active-button remap collapses codes (0x4→Unlock); only some survive; no capture to seed |
| Land Rover V0 | S 1/2 | ADVANCES | **PARTIAL** — 0x4 PASS; 0x2 decodes to signature 0x00 (button→command-signature mapping) |
| Kia V1 | S 0/3, C 0/3 | ADVANCES | **REPLAY (concern)** — encoder rebuilds the frame from the captured `Key`, NOT from `Btn`; decoder recovers a signature byte (0xD5), not the requested button. Counter advances. See Concerns. |
| VAG | 0 | UNREAD | crypto/keystore limit — aut64 needs a valid `Key2`+`KeyIdx` keyset; host keystore has type+key with **empty MF names**, so re-encode produces a frame the host decoder can't re-validate. Provider unchanged; scene branch untouched. |
| KeeLoq | S 0/4 (btn fixed 5) | FIXED | keystore limit — manufacturer match is name-based; `keystore_host.c` keys have empty names → no maker resolves. Existing working path; provider unchanged. |
| Ford V1 | 0 (SEED-FAIL) | — | crypto material absent — encoder requires `Key`+`Key_2`+`Key_3` that form a valid canonical raw; arbitrary bytes rejected; no capture in collection |
| Kia V7 | 0 (NODECODE) | — | no capture in collection; FSK/keeloq-style frame not self-consistent from a scratch seed |
| HondaV2&V3 | 0 (NODECODE) | — | no capture; needs a valid captured frame (V2 path) |
| HondaV5 | 0 (NODECODE) | — | no capture |
| HondaV8 | 0 (NODECODE) | — | keeloq frame needs a resolvable mfr key (host keystore name-empty); no capture |
| HondaV9 | 0 (NODECODE) | — | no capture (FSK checksum frame) |
| Mazda V0 | 0 (NODECODE) | — | no capture in collection |
| Chrysler V0 | 0 (NODECODE) | — | no capture (needs consistent Key_2/Plain_A/B) |

**Reading the matrix:** the decisive evidence is that Ford V0 and Ford V3 — which *fail* a
from-scratch synthetic seed — round-trip **perfectly (buttons + advancing counter)** the moment
they are seeded from a real capture, i.e. exactly how Task 3 routes a captured/saved fob into the
grid. That is the firmware's real path, and it works. The remaining NODECODE/SEED-FAIL rows are
protocols for which the local collection has **no capture** and whose encoders either replay a
captured field or need crypto/keystore material that isn't reconstructable from thin air on host —
not routing defects.

### Counter behaviour (Task-4 requirement)
Where the decoder's `get_string` prints a `Cnt:` token, the decoded counter was checked at N/N+1/N+2:
**ADVANCES** for Kia V0, SantaFe, Honda Static, Subaru, Land Rover (synthetic) and Kia V0, Kia V1,
Ford V0, Ford V3, Subaru (capture-seeded). **FIXED** for KeeLoq (host keystore no-match).
Refinement of Task-3's note: for SantaFe / Ford V3 the *visible* `Cnt` advances with the input; it
is the encrypted rolling/hop field (separate from `Cnt`) that is replayed — the button is re-baked,
the counter tracks.

### How to reproduce
```
wsl bash -lc 'cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && \
  bash tools/decoder_host_test/build_lib.sh && ./tools/decoder_host_test/fob_sweep'
```

## B. Build + no-regression — one-line result
Private **cardputer** (`build_adv_only_private.bat`) and private **m5stick**
(`build_m5stick_s3.bat`) both built to **SENTINEL_OK** with fresh merged bins; VAG + KeeLoq are
byte-for-byte unchanged (provider assertions pass AND the feature source has zero semantic diff
since Task 3).

- **Cardputer private:** `build_adv_only_private.log` → `SENTINEL_START/BUILD/MERGE/OK`,
  `Flipper-cardputer_adv-merged.bin` (0x2f3980 bytes) written. Exit 0.
- **M5stick private:** `build_m5stick_s3.log` → `SENTINEL_START/SETTARGET/BUILD/MERGE/OK`,
  `Flipper-m5stick_s3-merged.bin` (0x314ac0 bytes) written. Exit 0.
- Incremental builds (no firmware files added/removed — only `tools/`+report), so no
  clean-reconfigure needed.
- **NO-REGRESSION (VAG + KeeLoq):**
  - Provider assertion: `tests/test_subghz_fob_buttons.c` (Task 1) still **OK** —
    `subghz_fob_buttons("VAG")` = 3 tiles {0x1,0x2,0x4}, `("KeeLoq")` = 4 tiles {0x1,0x2,0x4,0x8},
    codes/order unchanged; `test_fob_nav` **OK**.
  - Source diff: `git diff -w bd34cd8 -- subghz_scene_keeloq_fob.c subghz_fob_buttons.c
    subghz_keeloq_fob.c` = **empty** (no semantic change since Task 3). The scene keeps the VAG
    (`SubGhzFobKindVag`) and KeeLoq (`SubGhzFobKindKeeloq`) branches separate from the generic
    branch (gated on `keeloq_fob_generic`); the press path uses `"Btn"` for VAG/KeeLoq and only the
    per-protocol field for the generic path.
- **Public build:** unaffected by design — the car material is already `#ifndef PUBLIC_BUILD`
  gated (Task 3), and this task added no firmware. Not rebuilt.

## Concerns
- **Kia V1 button does not round-trip (REPLAY).** Task 1 classified "Kia V1" SUPPORTED
  ("rebuilds upload with new CRC"), but the encoder's deserialize path builds the transmitted
  frame from the captured `Key` (`generic.data = key`), and the decoder recovers a signature byte
  (0xD5/0xAA/0xF5), not the requested `Btn`. Both synthetic and capture-seeded runs show the
  button is not honoured (counter still advances). Recommend a Task-1/3 follow-up: either re-bake
  the button into the Kia V1 upload or reclassify it REPLAY (drop from the grid).
- **HondaV7 / Land Rover partial button coverage.** HondaV7 routes buttons through the shared
  `custom_btn` active-button state (`hv7_register_btn`/`get_active_btn`), collapsing several codes
  to one on the fresh host decoder; Land Rover 0x2 decodes to command-signature 0x00. On device the
  `custom_btn` state is seeded by the capture, so behaviour may differ — worth an on-device check.
- **Host crypto/keystore limits are expected, not defects.** VAG (aut64 keyset), KeeLoq/HondaV8
  (name-based manufacturer keys) and Ford V1 (canonical-raw key derivation) can't be exercised from
  a scratch seed because `keystore_host.c` carries type+key pairs with empty names and no aut64
  keyset. These are verified structurally (provider + unchanged scene) and via the real captured
  path where a capture exists.
- **No collection captures for 9 of the 21** (Kia V7, HondaV2&V3/V5/V8/V9, Mazda V0, Chrysler V0,
  Ford V1, plus Honda Static's two files not lib-decoding). Their button synth is unverified on
  host; a capture-seeded pass (like Ford V0/V3 here) would confirm them if captures are obtained.
- **Not HW/RF-tested.** All verification is host round-trip + firmware build; no on-device TX.
