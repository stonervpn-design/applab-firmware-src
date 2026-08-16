#include "asset_pack.h"

#include <furi.h>
#include <storage/storage.h>

#define TAG "AssetPack"

// Manifest file that must exist inside a pack's Anims dir for the pack to
// be listed by asset_pack_enumerate() — mirrors the built-in
// /ext/dolphin/Anims/manifest.txt layout.
#define ASSET_PACK_MANIFEST_NAME "manifest.txt"

size_t asset_pack_enumerate(char names[][ASSET_PACK_NAME_LEN], size_t max) {
    furi_assert(names || max == 0);

    size_t count = 0;
    if(max == 0) return count;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);

    if(storage_dir_open(dir, ASSET_PACKS_DIR)) {
        FileInfo info;
        char entry_name[ASSET_PACK_NAME_LEN];

        while(count < max && storage_dir_read(dir, &info, entry_name, sizeof(entry_name))) {
            if(!file_info_is_dir(&info)) continue;
            if(!asset_pack_name_is_valid(entry_name)) continue;

            char manifest_path[sizeof(ASSET_PACKS_DIR) + ASSET_PACK_NAME_LEN + 32];
            snprintf(
                manifest_path,
                sizeof(manifest_path),
                "%s/%s/Anims/%s",
                ASSET_PACKS_DIR,
                entry_name,
                ASSET_PACK_MANIFEST_NAME);

            if(!storage_file_exists(storage, manifest_path)) continue;

            strlcpy(names[count], entry_name, ASSET_PACK_NAME_LEN);
            count++;
        }
    } else {
        FURI_LOG_D(TAG, "No asset packs directory (%s)", ASSET_PACKS_DIR);
    }

    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);

    return count;
}
