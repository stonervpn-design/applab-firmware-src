#pragma once
#include <stdint.h>
#include <stdbool.h>

// Hitag-2 stream cipher (48-bit LFSR + nonlinear filter f20). Pure integer ops,
// no heap / no PSRAM. Ported verbatim from the I.C. Wiener / Proxmark3 reference
// (the cipher was released into the public domain by its author) and verified
// byte-for-byte against the canonical MIKRON known-answer test.
//
// IMPORTANT: inputs are in the CIPHER convention, which is the FULL bit-reversal
// (byte order + bits within each byte) of the on-air LSB-first values. Use
// hitag2_rev32 / hitag2_rev48 to convert an on-air UID / key / nonce to cipher
// inputs. The decoder and any forge path MUST use the identical convention.

// Initialize cipher state from a 48-bit key, 32-bit serial (UID) and 32-bit IV/nonce.
uint64_t hitag2_init(uint64_t key, uint32_t serial, uint32_t iv);
// Emit the next keystream byte (bits assembled MSB-first), advancing state.
uint8_t hitag2_byte(uint64_t* state);
// Emit the next 32 keystream bits as a big-endian uint32 (the RKE 'ks' field).
uint32_t hitag2_ks32(uint64_t* state);

// Full bit reversal (bit + byte order) of a 32-/48-bit on-air value.
uint32_t hitag2_rev32(uint32_t v);
uint64_t hitag2_rev48(uint64_t v);

// Boot / first-run self-test: true iff the cipher reproduces the MIKRON vector.
bool hitag2_selftest(void);
