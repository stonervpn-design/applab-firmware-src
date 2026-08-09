# KeeLoq Slide / Manufacturer-Key Recovery — Design (2026-08-09)

PRIVATE-only, authorized research. Offline HOST tool: recover a 64-bit KeeLoq
MANUFACTURER key from device keys of the same make (Indesteege-Kasper-style slide
+ meet-in-the-middle), so any fob of that make can then be cloned. Design/spec
only.

## Goal

Given ≥2 device keys `kdev` (each = `normal_learning(serial, mfr_key)` = two
KeeLoq decrypts of serial-variants under the manufacturer key) with their known
serials, recover the 64-bit `mfr_key`. Then the existing keystore/clone path
covers every fob of that make.

## What exists

- `keeloq_common.c`: `encrypt`/`decrypt` (NLFSR, 528 rounds, NLF 0x3A5C742E),
  `normal_learning(serial, key)` = `(decrypt(serial|0x20000000, key)<<32) |
  decrypt(serial|0x60000000, key)`. This IS the relation the attack inverts.
- Host crypto tooling pattern (KAT / kat_decode / keeloq dir in Downloads).

## HARD CONSTRAINTS / honest feasibility (read first)

1. **Getting the INPUT is the real wall.** The slide attack needs the *device
   keys*, and a normal KeeLoq RKE fob transmits only the encrypted hop
   `encrypt(counter|btn, kdev)` — NEVER kdev. Extracting kdev without the mfr key
   requires either the ~2^16 chosen-plaintext attack (needs a bidirectional/IFF
   KeeLoq mode to inject chosen challenges — RKE is one-way, so this usually
   doesn't apply) or invasive fob readout. **For one-way UHF RKE, you generally
   cannot obtain device keys**, which makes this attack largely theoretical for
   the fobs this port captures.
2. **When it DOES apply** (you already hold ≥2 device keys of a make — e.g. from
   two decapped/known fobs, or an IFF-mode capture), the slide+MITM recovers the
   manufacturer key in feasible offline time (~2^40-class MITM with the KeeLoq
   round structure). That is the legitimate use.
3. **Low marginal value for this owner:** the private keystore already holds the
   manufacturer keys for the covered makes — recovering a *known* key is
   pointless; the tool only matters for an UNKNOWN make where you somehow already
   have device keys. Narrow.

## Approach (only worth building if input is obtainable)

- **Phase 0 — reality check (the gate).** Confirm a concrete scenario where you
  actually possess ≥2 device keys of an unknown make. If none exists, STOP —
  document the negative and do not build a tool with no feasible input.
- **Phase 1 — slide+MITM core (host).** Given (serialᵢ, kdevᵢ) pairs, invert
  `normal_learning` via meet-in-the-middle on the mfr key using the KeeLoq round
  structure; validate against synthetic data (pick mfr_key → derive device keys →
  recover it exactly). Rust/C for the MITM (mirror kat_decode).
- **Phase 2 — keystore write-back.** Emit the recovered mfr key in the private
  `keeloq_mfcodes_user` format for the firmware to use.

## Verification

Synthetic spine: choose mfr_key, generate device keys via the verified
`normal_learning`, recover mfr_key, assert equality. No real-world claim without
that working AND a real input scenario existing.

## Open questions for the owner

1. Do you have (or can obtain) ≥2 device keys of an UNKNOWN manufacturer? If not,
   this tool has no feasible input and should be shelved.
2. Is the intent an unknown gate/garage KeeLoq make (some support IFF/bidirectional
   modes where chosen-plaintext IS possible), rather than one-way car RKE?
