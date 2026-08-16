// Standalone host test for the asset-pack path resolver + name validator.
// Includes the real firmware header directly (both functions + the defines
// are header-only `static inline` / `#define` so this compiles the actual
// production code, not a copy) — no furi/storage dependency, so this stays
// host-portable. asset_pack_enumerate() (firmware-only, needs the storage
// API) is NOT covered here; see Task 2's firmware build verification.
//
// Run: wsl bash -lc "cd tools/asset_pack_host_test && make test"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../applications/services/desktop/helpers/asset_pack.h"

int main(void) {
    char b[64];

    // default resolution: empty / NULL / "Default" all -> built-in dolphin dir
    asset_pack_anims_dir("", b, sizeof b);
    assert(!strcmp(b, "/ext/dolphin"));
    asset_pack_anims_dir(NULL, b, sizeof b);
    assert(!strcmp(b, "/ext/dolphin"));
    asset_pack_anims_dir("Default", b, sizeof b);
    assert(!strcmp(b, "/ext/dolphin"));

    // named pack resolution
    asset_pack_anims_dir("Neon", b, sizeof b);
    assert(!strcmp(b, "/ext/asset_packs/Neon/Anims"));

    // validator: accept
    assert(asset_pack_name_is_valid("Neon"));

    // validator: reject
    assert(!asset_pack_name_is_valid(""));
    assert(!asset_pack_name_is_valid(NULL));
    assert(!asset_pack_name_is_valid("../x"));
    assert(!asset_pack_name_is_valid("a/b"));
    assert(!asset_pack_name_is_valid(
        "this_name_is_way_too_long_to_fit_in_the_buffer_limit")); // >= 32
    {
        // exactly ASSET_PACK_NAME_LEN chars (32) must be rejected (>= limit)
        char exact[ASSET_PACK_NAME_LEN + 1];
        memset(exact, 'a', ASSET_PACK_NAME_LEN);
        exact[ASSET_PACK_NAME_LEN] = '\0';
        assert(strlen(exact) == ASSET_PACK_NAME_LEN);
        assert(!asset_pack_name_is_valid(exact));

        // one shorter (31 chars) must be accepted
        exact[ASSET_PACK_NAME_LEN - 1] = '\0';
        assert(strlen(exact) == ASSET_PACK_NAME_LEN - 1);
        assert(asset_pack_name_is_valid(exact));
    }
    {
        // control characters rejected
        char ctrl[] = {'a', 'b', '\t', 'c', '\0'};
        assert(!asset_pack_name_is_valid(ctrl));
        char ctrl2[] = {'a', 0x7F, '\0'};
        assert(!asset_pack_name_is_valid(ctrl2));
    }
    // ".." embedded anywhere, not just as a full path segment
    assert(!asset_pack_name_is_valid("foo..bar"));
    assert(!asset_pack_name_is_valid(".."));

    // bounded: tiny buffer must truncate safely, not overflow
    char t[8];
    asset_pack_anims_dir("Neon", t, sizeof t);
    assert(t[7] == '\0');

    // bounded: out_size 0 / out NULL must not crash
    asset_pack_anims_dir("Neon", b, 0);
    asset_pack_anims_dir("Neon", NULL, sizeof b);

    printf("asset_pack_host_test: all assertions passed\n");
    return 0;
}
