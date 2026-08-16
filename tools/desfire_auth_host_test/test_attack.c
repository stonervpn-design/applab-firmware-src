// tools/desfire_auth_host_test/test_attack.c
#include "mf_desfire_attack.h"
#include <string.h>
#include <stdio.h>
extern int g_fail;
#define TCHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m); g_fail++;} else printf("ok: %s\n",m);}while(0)

// mock: AID 0x000001 key 0 is AES all-0x11; everything else fails
static bool mock_try(void* ctx, uint16_t aid, uint8_t kn, MfDesfireKeyType t, const uint8_t* key) {
    (void)ctx; static const uint8_t secret[16] = {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
                                                  0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    return aid == 0x000001 && kn == 0 && t == MfDesfireKeyTypeAes && memcmp(key, secret, 16) == 0;
}

void test_attack_sweep(void) {
    static const uint8_t z16[16] = {0}; static const uint8_t s16[16] =
        {0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    MfDesfireDictEntry dict[] = {
        { z16, MfDesfireKeyTypeAes }, { s16, MfDesfireKeyTypeAes },
    };
    uint16_t aids[] = { 0x000000, 0x000001 };
    MfDesfireFoundKey found[8];
    size_t n = mf_desfire_attack_sweep(mock_try, NULL, aids, 2, 1, dict, 2, found, 8, NULL, NULL);
    TCHECK(n == 1, "sweep finds exactly one key");
    TCHECK(found[0].aid == 0x000001 && found[0].key_no == 0, "sweep records correct slot");
    TCHECK(found[0].type == MfDesfireKeyTypeAes, "sweep records key type");
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
