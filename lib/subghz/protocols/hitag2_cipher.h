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

// ---- RKE forge (known-key) ----
// Compute the 32-bit RKE keystream 'ks' a genuine fob would transmit for a given
// full 28-bit rolling counter + 4-bit button, given the ON-AIR key (48b) and UID
// (32b). Nonce = (counter<<4)|button; on-air<->cipher bit-reversal handled inside.
uint32_t hitag2_rke_ks(uint64_t key_onair, uint32_t uid_onair, uint32_t counter, uint8_t button);

// Recover the FULL 28-bit counter from ONE captured frame (uid, low-10 counter bits,
// button, captured ks) given the key: brute-force the 18 unknown high bits (~2^18
// cipher runs, sub-second on-device). A hit simultaneously recovers the counter AND
// proves the key + nonce/bit-order convention are correct for THIS fob. Returns true
// and writes *out_counter on success; false if no counter matches (wrong key / variant).
bool hitag2_recover_counter(
    uint64_t key_onair,
    uint32_t uid_onair,
    uint16_t lctr10,
    uint8_t button,
    uint32_t captured_ks,
    uint32_t* out_counter);
