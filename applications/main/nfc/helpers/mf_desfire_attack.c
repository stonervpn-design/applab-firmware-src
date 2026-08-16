// mf_desfire_attack.c
//
// Pure-logic sweep engine: no poller/firmware includes. See
// mf_desfire_attack.h for the design rationale (host + firmware share this
// file unchanged, driven entirely through the injected try_auth callback).
#include "mf_desfire_attack.h"

#ifndef PUBLIC_BUILD

static const uint8_t k_key_des[8]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_key_2k3des[16]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_key_3k3des[24]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_key_aes[16]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const MfDesfireDictEntry k_dict_builtin[] = {
    {k_key_des, MfDesfireKeyTypeDes},
    {k_key_2k3des, MfDesfireKeyType2k3Des},
    {k_key_3k3des, MfDesfireKeyType3k3Des},
    {k_key_aes, MfDesfireKeyTypeAes},
};

size_t mf_desfire_dict_builtin(const MfDesfireDictEntry** out) {
    if (out) {
        *out = k_dict_builtin;
    }
    return sizeof(k_dict_builtin) / sizeof(k_dict_builtin[0]);
}

size_t mf_desfire_attack_sweep(
    MfDesfireTryAuthFn try_auth, void* ctx, const uint16_t* aids, size_t aid_count,
    uint8_t keys_per_app, const MfDesfireDictEntry* dict, size_t dict_count,
    MfDesfireFoundKey* out, size_t out_max, void (*progress)(void* p, size_t done, size_t total),
    void* progress_ctx) {
    if (!try_auth || !aids || !dict) {
        return 0;
    }

    size_t total = aid_count * (size_t)keys_per_app * dict_count;
    size_t done  = 0;
    size_t found = 0;

    for (size_t ai = 0; ai < aid_count; ai++) {
        uint16_t aid = aids[ai];
        for (uint8_t kn = 0; kn < keys_per_app; kn++) {
            for (size_t di = 0; di < dict_count; di++) {
                const MfDesfireDictEntry* entry = &dict[di];
                bool hit = try_auth(ctx, aid, kn, entry->type, entry->key);
                done++;
                if (progress) {
                    progress(progress_ctx, done, total);
                }
                if (hit) {
                    if (found < out_max) {
                        MfDesfireFoundKey* fk = &out[found];
                        fk->aid    = aid;
                        fk->key_no = kn;
                        fk->type   = entry->type;
                        size_t ksz = mf_desfire_key_size(entry->type);
                        for (size_t b = 0; b < ksz && b < MF_DESFIRE_CRYPTO_KEY_MAX; b++) {
                            fk->key[b] = entry->key[b];
                        }
                        for (size_t b = ksz; b < MF_DESFIRE_CRYPTO_KEY_MAX; b++) {
                            fk->key[b] = 0;
                        }
                    }
                    found++;
                    // Slot cracked — stop trying further dictionary entries
                    // against this (aid, key_no) and move to the next slot.
                    break;
                }
            }
        }
    }

    return found < out_max ? found : out_max;
}

#endif /* !PUBLIC_BUILD — DESFire dict-attack engine is PRIVATE-only */
