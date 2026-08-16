// Standalone host test for the security_wipe path-eligibility predicate.
// Includes the real firmware header directly (the predicate + whitelist are
// header-only `static inline` / `static const` so this compiles the actual
// production code, not a copy) — no furi/storage dependency, so this stays
// host-portable.
//
// Run: wsl bash -lc "cd tools/security_wipe_host_test && make test"

#include <assert.h>
#include <stdio.h>

#include "../../applications/services/desktop/helpers/security_wipe.h"

int main(void) {
    // eligible: exact whitelist matches
    assert(security_wipe_path_is_eligible("/ext/nfc"));
    assert(security_wipe_path_is_eligible("/ext/subghz"));
    assert(security_wipe_path_is_eligible("/ext/lfrfid"));
    assert(security_wipe_path_is_eligible("/ext/infrared"));
    assert(security_wipe_path_is_eligible("/ext/badusb"));
    assert(security_wipe_path_is_eligible("/ext/ibutton"));
    assert(security_wipe_path_is_eligible("/ext/rfid"));
    assert(security_wipe_path_is_eligible("/ext/u2f"));
    assert(security_wipe_path_is_eligible("/ext/apps_data"));

    // protected / not eligible
    assert(!security_wipe_path_is_eligible("/ext"));
    assert(!security_wipe_path_is_eligible("/ext/"));
    assert(!security_wipe_path_is_eligible("/ext/asset_packs"));
    assert(!security_wipe_path_is_eligible("/ext/dolphin"));
    assert(!security_wipe_path_is_eligible("/ext/apps"));
    assert(!security_wipe_path_is_eligible("/ext/update"));
    assert(!security_wipe_path_is_eligible("/ext/.obj"));
    assert(!security_wipe_path_is_eligible("/ext/nfc/../..")); // no traversal match
    assert(!security_wipe_path_is_eligible("/"));
    assert(!security_wipe_path_is_eligible(""));
    assert(!security_wipe_path_is_eligible("/ext/nfcX")); // exact match only
    assert(!security_wipe_path_is_eligible("/ext/nfc2"));
    assert(!security_wipe_path_is_eligible("/ext/NFC")); // case-sensitive
    assert(!security_wipe_path_is_eligible("ext/nfc")); // must be absolute
    assert(!security_wipe_path_is_eligible("/ext/nfc ")); // trailing space, no trim
    assert(!security_wipe_path_is_eligible(NULL)); // defensive: never crash, never eligible

    printf("security_wipe_host_test: all assertions passed\n");
    return 0;
}
