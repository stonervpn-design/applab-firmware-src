// tools/desfire_auth_host_test/test_attack.c
#include "mf_desfire_attack.h"
#include <string.h>
#include <stdio.h>
extern int g_fail;
#define TCHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m); g_fail++;} else printf("ok: %s\n",m);}while(0)

// mock: AID 0x000001 key 0 is AES, and accepts EITHER of two distinct keys
// (secret_a or secret_b) — this lets the sweep test prove "stop that slot
// on first hit" for real: if the dict places secret_a before secret_b and
// the `break` on first hit were ever deleted, the sweep would keep going
// and ALSO match secret_b for the very same (aid, key_no) slot, producing
// a second MfDesfireFoundKey for a slot that should have stopped after its
// first hit. Everything else fails.
static bool mock_try(void* ctx, uint16_t aid, uint8_t kn, MfDesfireKeyType t, const uint8_t* key) {
    (void)ctx;
    static const uint8_t secret_a[16] = {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
                                          0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    static const uint8_t secret_b[16] = {0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,
                                          0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22};
    return aid == 0x000001 && kn == 0 && t == MfDesfireKeyTypeAes &&
        (memcmp(key, secret_a, 16) == 0 || memcmp(key, secret_b, 16) == 0);
}

void test_attack_sweep(void) {
    static const uint8_t z16[16] = {0};
    static const uint8_t s16a[16] = {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
                                      0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    static const uint8_t s16b[16] = {0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,
                                      0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22};
    // s16a is the FIRST dict entry that matches (aid 0x000001, key_no 0);
    // s16b matches the SAME slot but comes LATER. A correct sweep stops at
    // s16a and never tries s16b for that slot.
    MfDesfireDictEntry dict[] = {
        { s16a, MfDesfireKeyTypeAes },
        { z16,  MfDesfireKeyTypeAes },
        { s16b, MfDesfireKeyTypeAes },
    };
    uint16_t aids[] = { 0x000000, 0x000001 };
    MfDesfireFoundKey found[8];
    size_t n = mf_desfire_attack_sweep(mock_try, NULL, aids, 2, 1, dict, 3, found, 8, NULL, NULL);
    TCHECK(n == 1, "sweep finds exactly one key (stops slot on first hit, doesn't also match s16b)");
    TCHECK(found[0].aid == 0x000001 && found[0].key_no == 0, "sweep records correct slot");
    TCHECK(found[0].type == MfDesfireKeyTypeAes, "sweep records key type");
    TCHECK(memcmp(found[0].key, s16a, 16) == 0,
        "sweep records the FIRST matching dict entry (s16a), not the later s16b");
}

// mock: key_no 0 / DES always succeeds, regardless of aid or key bytes —
// used to generate more hits than out_max so the clamp test below can
// prove both the return-value contract and the no-overrun guarantee.
static bool mock_try_always_key0_des(void* ctx, uint16_t aid, uint8_t kn, MfDesfireKeyType t,
                                      const uint8_t* key) {
    (void)ctx; (void)aid; (void)key;
    return kn == 0 && t == MfDesfireKeyTypeDes;
}

void test_attack_out_max_clamp(void) {
    static const uint8_t d8[8] = {0};
    MfDesfireDictEntry dict[] = { { d8, MfDesfireKeyTypeDes } };
    // 3 AIDs, each hits at key_no 0 -> 3 total finds, but out_max is 1.
    uint16_t aids[] = { 0x0000, 0x0001, 0x0002 };

    MfDesfireFoundKey out[2];
    // Sentinel in out[1] (outside the declared out_max=1 capacity) so we
    // can prove the sweep never writes past out[out_max - 1].
    memset(&out[1], 0xAA, sizeof(out[1]));
    MfDesfireFoundKey sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));

    size_t n = mf_desfire_attack_sweep(
        mock_try_always_key0_des, NULL, aids, 3, 1, dict, 1, out, 1, NULL, NULL);

    // Contract: mf_desfire_attack_sweep returns the true found-count,
    // clamped to out_max (there are 3 real hits, out_max is 1 -> returns 1).
    TCHECK(n == 1, "sweep return value is clamped to out_max (3 hits, out_max=1)");
    TCHECK(out[0].aid == 0x0000 && out[0].key_no == 0,
        "sweep still writes the one slot that fits within capacity");
    TCHECK(memcmp(&out[1], &sentinel, sizeof(sentinel)) == 0,
        "sweep never writes past out[out_max - 1] (sentinel untouched)");
}

void test_attack_dict_builtin(void) {
    const MfDesfireDictEntry* dict = NULL;
    size_t n = mf_desfire_dict_builtin(&dict);
    TCHECK(dict != NULL, "builtin dict returns non-null pointer");
    TCHECK(n == 4, "builtin dict has 4 entries");

    int saw_des = 0, saw_2k3des = 0, saw_3k3des = 0, saw_aes = 0;
    for (size_t i = 0; i < n; i++) {
        size_t ksz;
        switch (dict[i].type) {
            case MfDesfireKeyTypeDes:    ksz = 8;  saw_des = 1;    break;
            case MfDesfireKeyType2k3Des: ksz = 16; saw_2k3des = 1; break;
            case MfDesfireKeyType3k3Des: ksz = 24; saw_3k3des = 1; break;
            case MfDesfireKeyTypeAes:    ksz = 16; saw_aes = 1;    break;
            default: ksz = 0; break;
        }
        int all_zero = 1;
        for (size_t j = 0; j < ksz; j++) {
            if (dict[i].key[j] != 0x00) { all_zero = 0; break; }
        }
        TCHECK(all_zero, "builtin dict entry is all-zero default key");
    }
    TCHECK(saw_des && saw_2k3des && saw_3k3des && saw_aes,
        "builtin dict covers all four key types");
}
