#include "security_wipe.h"

#include <furi.h>
#include <storage/storage.h>

#define TAG "SecurityWipe"

// Recursively deletes every whitelisted user-capture dir on SD.
//
// security_wipe_path_is_eligible() (header, pure, host-tested) is the guard
// that defines the whitelist this iterates — this function does not perform
// any path logic of its own, it only iterates security_wipe_whitelist[] and
// calls the real recursive-remove storage API on each entry.
void security_wipe_user_data(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    for(size_t i = 0; i < SECURITY_WIPE_WHITELIST_COUNT; i++) {
        const char* path = security_wipe_whitelist[i];
        bool ok = storage_simply_remove_recursive(storage, path);
        FURI_LOG_I(TAG, "wipe %s: %s", path, ok ? "ok" : "failed");
    }

    furi_record_close(RECORD_STORAGE);
}
