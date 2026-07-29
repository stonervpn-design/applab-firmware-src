#include "mfkey32_recover.h"

#include <stddef.h>

#include <crapto1.h>

bool mfkey32_recover_cancellable(
    uint32_t uid,
    uint32_t nt0,
    uint32_t nr0_enc,
    uint32_t ar0_enc,
    uint32_t nt1,
    uint32_t nr1_enc,
    uint32_t ar1_enc,
    uint64_t* out_key,
    Mfkey32CancelFn cancel,
    void* cancel_ctx) {
    if(!out_key) return false;

    // Standard mfkey32 (two-auth, same-key) recovery. Port of the recover loop
    // from noproto/FlipperMfkey (fap/mfkey32.c). See crapto1.{c,h}.
    uint32_t p64 = crapto1_prng_successor(nt0, 64);
    uint32_t p64b = crapto1_prng_successor(nt1, 64);

    Crapto1Params p = {
        .key = 0,
        .nr0_enc = nr0_enc,
        .uid_xor_nt0 = uid ^ nt0,
        .uid_xor_nt1 = uid ^ nt1,
        .nr1_enc = nr1_enc,
        .p64b = p64b,
        .ar1_enc = ar1_enc,
    };

    if(!crapto1_recover(&p, ar0_enc ^ p64, (Crapto1CancelFn)cancel, cancel_ctx)) {
        return false;
    }

    *out_key = p.key;
    return true;
}

bool mfkey32_recover(
    uint32_t uid,
    uint32_t nt0,
    uint32_t nr0_enc,
    uint32_t ar0_enc,
    uint32_t nt1,
    uint32_t nr1_enc,
    uint32_t ar1_enc,
    uint64_t* out_key) {
    return mfkey32_recover_cancellable(
        uid, nt0, nr0_enc, ar0_enc, nt1, nr1_enc, ar1_enc, out_key, NULL, NULL);
}

bool mfkey32_recover_selftest(void) {
    // Published vector from noproto/FlipperMfkey dev/standard_tests/
    // a0a1a2a3a4a5_only.log (line 1). Key A0A1A2A3A4A5.
    uint64_t key = 0;
    bool ok = mfkey32_recover(
        0x2a234f80, // uid / cuid
        0xbe771bb5, // nt0
        0x2328dd4a, // nr0_enc
        0xcaefa8a9, // ar0_enc
        0x2be39d62, // nt1
        0x27c7fe47, // nr1_enc
        0x3c270451, // ar1_enc
        &key);
    return ok && (key == 0xA0A1A2A3A4A5ULL);
}
