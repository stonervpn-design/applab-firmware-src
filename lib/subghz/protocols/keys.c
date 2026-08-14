#include "keys.h"

#define KIA_KEY1 10u
#define KIA_KEY2 11u
#define KIA_KEY3 12u
#define KIA_KEY4 13u

uint64_t kia_mf_keys[KIA_MF_KEY_MAX] = {0};
uint8_t kia_mf_key_count = 0;

uint64_t kia_mf_key = 0;
uint64_t kia_v6_a_key = 0;
uint64_t kia_v6_b_key = 0;
uint64_t kia_v5_key = 0;

// Append a KIA_KEY1 manufacturer key, skipping exact duplicates and respecting
// the fixed capacity, so the decoder can try every provisioned Kia key.
static void kia_mf_key_add(uint64_t key) {
    for(uint8_t i = 0; i < kia_mf_key_count; i++) {
        if(kia_mf_keys[i] == key) return;
    }
    if(kia_mf_key_count < KIA_MF_KEY_MAX) {
        kia_mf_keys[kia_mf_key_count++] = key;
    }
}

void protopirate_keys_load(SubGhzEnvironment* environment) {
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    // Reset the accumulated Kia key set: this function may run more than once.
    kia_mf_key_count = 0;
    for(uint8_t i = 0; i < KIA_MF_KEY_MAX; i++) kia_mf_keys[i] = 0;
    // Load keys from secure keystore
            for
                M_EACH(manufacture_code, *subghz_keystore_get_data(keystore), SubGhzKeyArray_t) {
                    switch(manufacture_code->type) {
                    case KIA_KEY1:
                        kia_mf_key_add(manufacture_code->key);
                        kia_mf_key = manufacture_code->key;
                        break;
                    case KIA_KEY2:
                        kia_v6_a_key = manufacture_code->key;
                        break;
                    case KIA_KEY3:
                        kia_v6_b_key = manufacture_code->key;
                        break;
                    case KIA_KEY4:
                        kia_v5_key = manufacture_code->key;
                        break;
                    }
                }
}

uint64_t get_kia_mf_key() {
    return kia_mf_key;
}

uint8_t get_kia_mf_key_count(void) {
    return kia_mf_key_count;
}

uint64_t get_kia_mf_key_at(uint8_t index) {
    if(index >= kia_mf_key_count) return 0;
    return kia_mf_keys[index];
}

uint64_t get_kia_v6_keystore_a() {
    return kia_v6_a_key;
}

uint64_t get_kia_v6_keystore_b() {
    return kia_v6_b_key;
}

uint64_t get_kia_v5_key() {
    return kia_v5_key;
}
