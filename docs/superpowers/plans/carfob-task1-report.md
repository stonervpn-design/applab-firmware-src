# Car-fob emulate-all-protocols — Task 1 report

**Task:** Audit `lib/subghz/protocols/*.c` car keyfobs + build a generic button-table provider.
**Branch:** feat/keeloq-no-desync-clone
**Status:** DONE. Host test PASS. Committed (no push).

## Deliverables
- `applications/main/subghz/helpers/subghz_fob_buttons.h` — self-contained
  (`stdint`/`stddef`/`stdbool` only), `SubGhzFobButton{uint8_t code; const char* name;}`,
  `subghz_fob_buttons(name,&n)`, `subghz_fob_protocol_supported(name)`.
- `applications/main/subghz/helpers/subghz_fob_buttons.c` — audit comment block +
  21 per-protocol `{code,name}[]` tables + name-string dispatch.
- `tests/test_subghz_fob_buttons.c` — single-TU host test (includes the .c).

## Classification criteria
- **SUPPORTED** (in provider grid): encoder deserialize reads an INDEPENDENT button
  field (`Btn`/`Button`/`BtnLo`) AND rebuilds the transmitted frame from it, AND the
  protocol exposes an enumerable button code→name map.
- **SYNTH-NO-MAP**: independent button read + frame rebuild, but no name map (button is
  a free byte / KeeLoq nibble). TX-capable, but not enumerable → excluded from the grid.
- **REPLAY**: encoder replays the captured Key/rolling code, or the button is only
  selectable via the `custom_btn` UI override (not a `.sub` field) → plain transmitter.
- **UNSUPPORTED**: no functional encoder (NULL/stub, or no Send flag).

## Audit table (car keyfobs)
| Protocol name | Class | Grid buttons |
|---|---|---|
| VAG | SUPPORTED | 3 — 0x1/0x2/0x4 (existing view) |
| KeeLoq | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 (existing view) |
| Kia V0 | SUPPORTED | 3 — 0x1/0x2/0x3 |
| Kia V1 | SUPPORTED | 3 — 0x1/0x2/0x3 |
| Kia V2 | SYNTH-NO-MAP | — |
| Kia V3/V4 | SYNTH-NO-MAP | — |
| Kia V5 | SYNTH-NO-MAP | — |
| Kia V6 | SYNTH-NO-MAP | — |
| Kia V7 | SUPPORTED | 4 — 0x1/0x2/0x3/0x8 |
| Hyundai/Kia RIO | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 |
| Hyundai TL | REPLAY | — |
| SantaFe 13-16 | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 |
| Ford V0 | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 |
| Ford V1 | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 (0x0 Sync omitted) |
| Ford V2 | REPLAY | — (button in Key) |
| Ford V3 | SUPPORTED | 2 — 0x1/0x2 |
| Honda Static | SUPPORTED | 6 — 0x1/0x2/0x4/0x5/0x8/0x9 |
| HondaV2&V3 | SUPPORTED | 3 — 0x20/0x40/0x60 (V2 path; V3=Key replay) |
| HondaV4 | REPLAY | — (custom_btn override) |
| HondaV5 | SUPPORTED | 2 — 0x21/0x22 (field "Button") |
| HondaV6 | REPLAY | — (Key replay) |
| HondaV7 | SUPPORTED | 6 — 0x0/0x1/0x2/0x4/0x8/0x9 (field "Button") |
| HondaV8 | SUPPORTED | 6 — 0x0/0x1/0x2/0x4/0x8/0x9 (field "Button") |
| HondaV9 | SUPPORTED | 6 — 0x0/0x1/0x2/0x4/0x8/0x9 (field "Button") |
| HondaV10 | REPLAY | — (raw Key replay) |
| Mazda V0 | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 |
| Mazda V1 | REPLAY | — (button in Key) |
| Mazda V2 | REPLAY | — (custom_btn override) |
| MazdaSiemens | SYNTH-NO-MAP | — |
| Subaru | SUPPORTED | 4 — 0x0/0x1/0x2/0x3 |
| Fiat V1 | SUPPORTED | 4 — 0x1/0x2/0x4/0x8 (needs Hitag2 Key) |
| FIAT SPA | REPLAY | — |
| Fiat V0 | REPLAY | — |
| Fiat Multi (V2/V3/V5) | REPLAY | — (custom_btn override) |
| Ducato V0 | REPLAY | — (custom_btn override) |
| Chrysler V0 | SUPPORTED | 2 — 0x1/0x2 |
| Jeep V0 | REPLAY | — (16-bit codes in Key) |
| Peugot V0 | REPLAY | — |
| Opel V0 | REPLAY | — |
| LDV V0 | UNSUPPORTED | — (stub encoder, no Send) |
| Land Rover V0 | SUPPORTED | 2 — 0x2/0x4 |
| ChevroletV0 | REPLAY | — |
| Nissan V0 | REPLAY | — |
| Toyota V0 | REPLAY | — |
| Lexus V0 | REPLAY | — |
| Audi V1 | REPLAY | — (custom_btn override) |
| Audi V2 | REPLAY | — |
| Audi V3 | REPLAY | — (button in Key) |
| BMW_v2 | REPLAY | — |
| BMW_v3 | REPLAY | — (button in Key) |
| BMW CAS4 | UNSUPPORTED | — (NULL encoder) |
| Renault V1..V4 | REPLAY | — (encrypted rolling replay) |
| Mitsubishi V0 | SYNTH-NO-MAP | — |
| Mitsubishi V1 | REPLAY | — (button in Key) |
| Porsche Touareg | SYNTH-NO-MAP | — |
| Sheriff CFM | SYNTH-NO-MAP | — (codes 0x10/0x20/0x40/0x80, no names) |
| Nord ICE | REPLAY | — |
| Star Line | SYNTH-NO-MAP | — (KeeLoq synth, no name map) |
| Scher-Khan | REPLAY | — (verbatim bitstream, Btn cosmetic) |

**21 SUPPORTED** protocols in the provider (incl. the pre-existing VAG + KeeLoq layouts,
now sourced from this single provider). Codes/names are transcribed verbatim from each
protocol's own map — none invented.

## Test
```
wsl bash -lc 'cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && \
  gcc -Wall -Wextra -I applications/main/subghz/helpers \
  tests/test_subghz_fob_buttons.c -o /tmp/tsfb && /tmp/tsfb'
```
Result: **PASS** (`test_subghz_fob_buttons OK`), clean `-Wall -Wextra`.
Asserts: VAG→3 (0x1/0x2/0x4); KeeLoq→4 (0x1/0x2/0x4/0x8); Honda Static→6; HondaV7→6;
Kia V0→3; Ford V0→4; unknown→(NULL,0); `subghz_fob_protocol_supported` true for those,
false for replay-only "Scher-Khan" and for NULL/unknown.

## Concerns / notes for later tasks
- **Field name differs by family (Task 3 write path):** most protocols read `Btn`; Honda
  V5/V7/V8/V9 read `Button`; Honda V2&V3 reads `BtnLo`. The generic press→TX path must
  write the field name matching the protocol, not a hardcoded `Btn`.
- **HondaV2&V3 is partial:** the V2 sub-path is button-independent (grid works); the V3
  sub-path bakes the button into Key (replay). Same protocol-name string covers both, so a
  V3 capture routed into the grid would mis-synth. Task 3 may want to gate on the decoded
  sub-variant.
- **Fiat V1** only rebuilds when a valid `Hitag2 Key` field is present in the `.sub`
  (deserialize aborts otherwise) — routing should confirm the field exists.
- **SantaFe / Ford V3 / Fiat V1** rebuild the frame from the button but replay the rolling
  field (no counter increment on this side) — fine for the button grid; counter-advance is
  Task 3's persist step.
- **SYNTH-NO-MAP set** (Kia V2/V3-V4/V5/V6, MazdaSiemens, Mitsubishi V0, Porsche Touareg,
  Sheriff CFM, Star Line) can TX an arbitrary button but ships no name table. If desired
  later, give them explicit `{code,name}` tables (e.g. Sheriff CFM 0x10/0x20/0x40/0x80).
- **Kia V7** maps both 0x3 and 0x8 to "Trunk" (kept verbatim → two Trunk tiles).
- Provider is host-only data (no furi); the view/scene wiring is Tasks 2–3.
