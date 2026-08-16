#pragma once

// Asset Packs: swappable dolphin-animation sets.
//
// Packs live on the SD card at /ext/asset_packs/<Name>/, mirroring the
// built-in /ext/dolphin layout (Anims/manifest.txt + frame files). This
// header holds:
//
//   - asset_pack_anims_dir()      resolve a pack name -> its Anims dir
//   - asset_pack_name_is_valid()  validate a pack name (path-injection guard)
//
// Both are intentionally kept header-only / furi-free (`static inline` /
// `#define`) so they can be host-compiled and unit-tested in isolation —
// see tools/asset_pack_host_test/. This is the SAME code the firmware links
// against (not a copy): asset_pack.c includes this header too.
//
//   - asset_pack_enumerate()      firmware-only pack listing (needs Storage)
//
// is declared here but implemented in asset_pack.c since it needs the real
// storage API.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max length (including NUL) of a pack name, e.g. as stored in
// DesktopSettings.asset_pack and in the names[][] buffers passed to
// asset_pack_enumerate(). Keep in sync with DesktopSettings's field size.
#define ASSET_PACK_NAME_LEN 32

// Root directory on the SD card under which asset packs live, one
// subdirectory per pack (named after the pack).
#define ASSET_PACKS_DIR "/ext/asset_packs"

// Built-in animation directory used when no pack (or the sentinel name
// "Default") is selected.
#define ASSET_PACK_BUILTIN_ANIMS_DIR "/ext/dolphin"

// PURE. No furi/storage includes, no filesystem access.
//
// Resolves `pack_name` to the directory that holds its Anims (manifest.txt
// + frame files):
//   - NULL, "" (empty), or "Default" -> ASSET_PACK_BUILTIN_ANIMS_DIR
//   - anything else                  -> ASSET_PACKS_DIR "/" <pack_name> "/Anims"
//
// Bounded via snprintf: never overflows `out`, and always leaves `out`
// NUL-terminated (snprintf's own truncate-and-terminate guarantee) as long
// as out_size > 0. No-ops if `out` is NULL or `out_size` is 0.
static inline void asset_pack_anims_dir(const char* pack_name, char* out, size_t out_size) {
    if(out == NULL || out_size == 0) return;

    if(pack_name == NULL || pack_name[0] == '\0' || strcmp(pack_name, "Default") == 0) {
        snprintf(out, out_size, "%s", ASSET_PACK_BUILTIN_ANIMS_DIR);
    } else {
        snprintf(out, out_size, "%s/%s/Anims", ASSET_PACKS_DIR, pack_name);
    }
}

// PURE. No furi/storage includes, no filesystem access.
//
// Path-injection guard: returns true iff `name` is safe to splice as the
// single path segment ASSET_PACKS_DIR "/" <name> "/Anims". Rejects:
//   - NULL or empty (that's the "default" sentinel, handled separately by
//     asset_pack_anims_dir(), never a valid *named* pack)
//   - length >= ASSET_PACK_NAME_LEN (must fit the stored/enumerated field)
//   - any '/' (would escape the single path segment / add subdirectories)
//   - any ".." substring anywhere in the name (traversal, not just as a
//     full path segment — e.g. "foo..bar" is also rejected defensively)
//   - any control character, 0x00-0x1F or 0x7F (DEL)
static inline bool asset_pack_name_is_valid(const char* name) {
    if(name == NULL || name[0] == '\0') return false;

    size_t len = strlen(name);
    if(len >= ASSET_PACK_NAME_LEN) return false;

    for(size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if(c == '/' || c < 0x20 || c == 0x7F) return false;
    }

    if(strstr(name, "..") != NULL) return false;

    return true;
}

// Firmware-only: enumerates asset packs present on the SD card. Lists
// subdirectories of ASSET_PACKS_DIR that contain an Anims/manifest.txt
// file, writing up to `max` names (each ASSET_PACK_NAME_LEN bytes,
// NUL-terminated) into `names`, and returns the number written. Names that
// don't pass asset_pack_name_is_valid() (e.g. hand-edited/corrupt SD
// contents) are skipped rather than surfaced. Implemented in asset_pack.c
// (needs furi + the Storage API).
size_t asset_pack_enumerate(char names[][ASSET_PACK_NAME_LEN], size_t max);

#ifdef __cplusplus
}
#endif
