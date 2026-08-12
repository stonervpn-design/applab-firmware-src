// Host keystore shim: a valid SubGhzKeystore, loaded from the firmware's real
// plaintext keystore asset (keeloq_mfcodes_user, Encryption: 0), so
// keeloq-family decoders (star_line, vag, keys.c) link and iterate safely on
// the PC. Real manufacturer names are loaded (see keystore_entries.h), so
// name-based lookups (star_line/scher_khan, and the Task 1 registry's
// KIA/Beninca-by-name classification) now resolve correctly, not just
// type-based ones.
// Avoids compiling the real keystore.c (storage + esp_heap_caps + AES) --
// that file loads a SEPARATE, actually-AES-encrypted asset (keeloq_mfcodes,
// Encryption: 1); this shim instead mirrors keeloq_mfcodes_user, the
// plaintext asset the real app also loads unencrypted.
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/subghz_keystore_i.h>
#include <core/furi_string.h>
#include "keystore_entries.h"

static SubGhzKeystore g_ks;
static int g_init = 0;

static void ensure(void) {
    if(!g_init) {
        SubGhzKeyArray_init(g_ks.data);
        // Load the real manufacturer keys (type+key+name) so keeloq decoders
        // that gate on protopirate_keys_load() (kia_v2/v3_v4/v5/v6...) AND
        // name-based decoders/classifiers (star_line, the Task 1 registry)
        // both validate.
        for(int i = 0; i < KS_ENTRIES_N; i++) {
            SubGhzKey* k = SubGhzKeyArray_push_new(g_ks.data);
            k->name = furi_string_alloc_set_str(KS_ENTRIES[i].name);
            k->key = KS_ENTRIES[i].key;
            k->type = KS_ENTRIES[i].type;
        }
        g_init = 1;
    }
}

SubGhzKeystore* subghz_environment_get_keystore(SubGhzEnvironment* environment) {
    (void)environment;
    ensure();
    return &g_ks;
}

SubGhzKeyArray_t* subghz_keystore_get_data(SubGhzKeystore* instance) {
    (void)instance;
    ensure();
    return &g_ks.data;
}

bool subghz_keystore_raw_get_data(const char* file_name, size_t offset, uint8_t* data, size_t len) {
    (void)file_name;
    (void)offset;
    (void)data;
    (void)len;
    return false;
}
