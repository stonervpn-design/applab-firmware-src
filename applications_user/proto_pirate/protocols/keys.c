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

static bool kia_mf_key_loaded = false;
static bool kia_v6_a_key_loaded = false;
static bool kia_v6_b_key_loaded = false;
static bool kia_v5_key_loaded = false;

// Append a KIA_KEY1 manufacturer key, skipping exact duplicates and respecting
// the fixed capacity. Mirrors the newest key into the legacy kia_mf_key slot so
// the encoder's get_kia_mf_key() keeps working unchanged.
static void kia_mf_key_add(uint64_t key) {
    for(uint8_t i = 0; i < kia_mf_key_count; i++) {
        if(kia_mf_keys[i] == key) return;
    }
    if(kia_mf_key_count < KIA_MF_KEY_MAX) {
        kia_mf_keys[kia_mf_key_count++] = key;
    }
}

static void protopirate_keys_reset(void) {
    for(uint8_t i = 0; i < KIA_MF_KEY_MAX; i++) kia_mf_keys[i] = 0;
    kia_mf_key_count = 0;
    kia_mf_key = 0;
    kia_v6_a_key = 0;
    kia_v6_b_key = 0;
    kia_v5_key = 0;
    kia_mf_key_loaded = false;
    kia_v6_a_key_loaded = false;
    kia_v6_b_key_loaded = false;
    kia_v5_key_loaded = false;
}

bool protopirate_keys_load(SubGhzEnvironment* environment) {
    protopirate_keys_reset();
    if(!environment) {
        return false;
    }

    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    if(!keystore) {
        return false;
    }

    SubGhzKeyArray_t* key_data = subghz_keystore_get_data(keystore);
    if(!key_data) {
        return false;
    }

    for
        M_EACH(manufacture_code, *key_data, SubGhzKeyArray_t) {
            switch(manufacture_code->type) {
            case KIA_KEY1:
                kia_mf_key_add(manufacture_code->key);
                kia_mf_key = manufacture_code->key;
                kia_mf_key_loaded = true;
                break;
            case KIA_KEY2:
                kia_v6_a_key = manufacture_code->key;
                kia_v6_a_key_loaded = true;
                break;
            case KIA_KEY3:
                kia_v6_b_key = manufacture_code->key;
                kia_v6_b_key_loaded = true;
                break;
            case KIA_KEY4:
                kia_v5_key = manufacture_code->key;
                kia_v5_key_loaded = true;
                break;
            default:
                break;
            }
        }

    return kia_mf_key_loaded || kia_v6_a_key_loaded || kia_v6_b_key_loaded || kia_v5_key_loaded;
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

bool protopirate_keys_has_kia_mf_key(void) {
    return kia_mf_key_loaded;
}

bool protopirate_keys_has_kia_v6_keystore_a(void) {
    return kia_v6_a_key_loaded;
}

bool protopirate_keys_has_kia_v6_keystore_b(void) {
    return kia_v6_b_key_loaded;
}

bool protopirate_keys_has_kia_v5_key(void) {
    return kia_v5_key_loaded;
}
