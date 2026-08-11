// Host keystore shim: an EMPTY, valid SubGhzKeystore so keeloq-family decoders
// (star_line, vag, keys.c) link and iterate safely on the PC. No manufacturer
// keys are loaded, so KeeLoq manufacturer-key resolution won't identify a maker
// (rolling-code decrypt is a no-match), but the frame still decodes structurally.
// Avoids compiling the real keystore.c (storage + esp_heap_caps + AES).
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
        // Load the real manufacturer keys (type+key) so keeloq decoders that
        // gate on protopirate_keys_load() (kia_v2/v3_v4/v5/v6...) validate.
        // Names are empty (kat blob dropped them) -> name-based lookups
        // (star_line/scher_khan) still won't match; type-based (Kia) will.
        for(int i = 0; i < KS_ENTRIES_N; i++) {
            SubGhzKey* k = SubGhzKeyArray_push_new(g_ks.data);
            k->name = furi_string_alloc(); // empty, non-NULL (star_line derefs it)
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
