# crypto_kat — firmware crypto known-answer tests

Regression guard for the firmware's cryptographic primitives. Compiles the
**real, unmodified** `lib/subghz/protocols/*.c` cipher sources on the host and
checks them against authoritative published vectors plus structural invariants.

The point: catch a broken cipher **before** it ships. The KeeLoq core once had an
Xtensa shift-UB bug (`nl_extend`) that silently corrupted ~25% of keys with no
compile error and no crash — exactly the class of defect a known-answer test
turns into a loud, immediate failure.

## Run

Needs a host C compiler + make (this tree's tooling uses WSL gcc):

```sh
# from tools/crypto_kat/
make test        # builds ./crypto_kat and runs it; exits non-zero on any failure
make clean
```

On Windows, from the repo the rest of the host harnesses use:

```sh
wsl -e bash -lc 'cd /mnt/c/.../src_m5stick_s3/tools/crypto_kat && make test'
```

Exit code is non-zero if any check fails, so it can gate CI or a pre-flash check.

## What is tested & vector provenance

| Primitive | Source | Anchor |
|-----------|--------|--------|
| KeeLoq (encrypt/decrypt, 528-round) | `keeloq_common.c` | `github.com/hadipourh/KeeLoq` README vectors, independently reproduced |
| KeeLoq Normal Learning | `keeloq_common.c` | golden derived from the validated decrypt primitive |
| AES-128 (encrypt/decrypt) | `aes_common.c` | NIST FIPS-197 Appendix C.1 |
| Hitag2 (keystream + RKE forge) | `hitag2_cipher.c` | canonical MIKRON vector (I.C. Wiener / Proxmark3) + forge↔counter-recovery round-trip |
| AUT64 (block enc/dec) | `aut64.c` | invertibility + key-sensitivity + a behavior snapshot (no config-matched public vector exists) |

### Updating the AUT64 snapshot

AUT64 has no public known-answer vector for this key layout, so the test pins the
current ciphertext as a **change detector** (alongside the always-valid
invertibility and key-sensitivity invariants). If you *intentionally* change the
AUT64 implementation, the test prints the new `aut64 ct = …`; copy those 8 bytes
into `snap[]` in `test_crypto_kat.c`. If it changed **without** an intended edit,
that is a regression — investigate, do not just re-pin.

## Design notes

`keeloq_common.h` and `aes_common.h` use a quoted `#include "base.h"` that, in the
real tree, drags in the whole `furi_hal`/`flipper_format` stack. The pure crypto
uses none of it, so the Makefile stages byte-identical **copies** of the sources
into `stage/` next to a stub `base.h` (and provides stub `furi.h` / `m-array.h`),
letting the real crypto compile with zero furi dependency. **The cipher sources
are never edited** — only copied.
