#include "../lib/subghz/protocols/hitag2_cipher.c"
#include <assert.h>
#include <stdio.h>

static uint32_t ks32_from(uint64_t key, uint32_t serial, uint32_t iv) {
    uint64_t s = hitag2_init(key, serial, iv);
    return hitag2_ks32(&s);
}

int main(void) {
    // V1 MIKRON (authoritative: I.C. Wiener / Proxmark3 / RFIDler self-test vector).
    assert(hitag2_selftest());
    assert(ks32_from(0x4AD292B272F2ull, 0x96EAC292u, 0x4EA276A6u) == 0xD7237FCEu);

    // V3 all-ones (non-degenerate cross-check).
    assert(ks32_from(0xFFFFFFFFFFFFull, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFD67u);

    // V4 mixed (cross-check).
    assert(ks32_from(0x0123456789ABull, 0x89ABCDEFu, 0x01234567u) == 0x1A71DC67u);

    // Degenerate wiring check: all-zero is a fixed point -> zero keystream.
    assert(ks32_from(0, 0, 0) == 0);

    // On-air -> cipher bit-reversal (the R3 gotcha): the MIKRON on-air key/serial
    // reverse to the pre-reversed integers fed into hitag2_init.
    assert(hitag2_rev48(0x4F4E4D494B52ull) == 0x4AD292B272F2ull);
    assert(hitag2_rev32(0x49435769u) == 0x96EAC292u);
    assert(hitag2_rev32(0x656E4572u) == 0x4EA276A6u);

    printf("test_hitag2_cipher OK (MIKRON + all-FF + mixed + rev)\n");
    return 0;
}
