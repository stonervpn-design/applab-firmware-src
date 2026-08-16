#pragma once

// Anti-theft SD user-data wipe helper.
//
// security_wipe_path_is_eligible() is the safety-critical guard that keeps
// security_wipe_user_data() (in security_wipe.c) from ever touching anything
// but the whitelisted user-capture directories below. It is intentionally
// kept header-only / furi-free so it can be host-compiled and unit-tested in
// isolation — see tools/security_wipe_host_test/. This is the SAME code the
// firmware links against (not a copy): security_wipe.c includes this header
// too.
//
// Default-deny, exact-match only: a path is eligible iff it is byte-for-byte
// identical to one of the whitelist entries. Nothing is derived, trimmed,
// normalized, or resolved from the input — no prefix/substring matching, no
// path-traversal collapsing. Anything not an exact whitelist hit (including
// the explicit protected paths below, "/", "", and any traversal string) is
// NOT eligible.

#include <stddef.h>
#include <string.h>
#include <stdbool.h>

// User-capture directories that ARE eligible for wipe.
static const char* const security_wipe_whitelist[] = {
    "/ext/nfc",
    "/ext/subghz",
    "/ext/lfrfid",
    "/ext/infrared",
    "/ext/badusb",
    "/ext/ibutton",
    "/ext/rfid",
    "/ext/u2f",
    "/ext/apps_data",
};

#define SECURITY_WIPE_WHITELIST_COUNT \
    (sizeof(security_wipe_whitelist) / sizeof(security_wipe_whitelist[0]))

// Explicitly protected paths — documented here as defense-in-depth even
// though exact-match-only whitelist lookup below would already reject them.
// Never eligible, no matter what.
static const char* const security_wipe_protected[] = {
    "/ext",
    "/ext/",
    "/ext/asset_packs",
    "/ext/dolphin",
    "/ext/apps",
    "/ext/update",
    "/ext/.obj",
};

#define SECURITY_WIPE_PROTECTED_COUNT \
    (sizeof(security_wipe_protected) / sizeof(security_wipe_protected[0]))

// PURE. No furi/storage includes, no filesystem access, no path parsing.
// Returns true iff `path` is byte-for-byte equal to a whitelist entry.
static inline bool security_wipe_path_is_eligible(const char* path) {
    if(path == NULL) return false;

    // Defense-in-depth: explicit protected-path reject first.
    for(size_t i = 0; i < SECURITY_WIPE_PROTECTED_COUNT; i++) {
        if(strcmp(path, security_wipe_protected[i]) == 0) return false;
    }

    for(size_t i = 0; i < SECURITY_WIPE_WHITELIST_COUNT; i++) {
        if(strcmp(path, security_wipe_whitelist[i]) == 0) return true;
    }

    return false;
}

// Firmware-only: recursively deletes every whitelisted user-capture dir on
// SD via the storage API. Implemented in security_wipe.c (needs furi).
void security_wipe_user_data(void);
