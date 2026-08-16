// Standalone host test for the dolphin 30-level threshold table + generalized
// level functions. Pulls dolphin_level.c in as a single translation unit so
// the (intentionally file-scope) threshold table is directly assertable.
//
// Run: wsl bash -lc "cd tools/dolphin_level_host_test && make test"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../applications/services/dolphin/helpers/dolphin_level.c"

int main(void) {
    // table sanity
    assert(DOLPHIN_LEVEL_COUNT == 30);
    for(int i = 1; i < 29; i++) {
        assert(dolphin_level_thresholds[i] > dolphin_level_thresholds[i - 1]); // strictly increasing
    }
    assert(dolphin_level_thresholds[0] == 300);
    assert(dolphin_level_thresholds[28] >= 55000 && dolphin_level_thresholds[28] <= 65000);

    // level boundaries
    assert(dolphin_get_level(0) == 1);
    assert(dolphin_get_level(300) == 1);
    assert(dolphin_get_level(301) == 2);
    assert(dolphin_get_level(dolphin_level_thresholds[28]) == 29);
    assert(dolphin_get_level(dolphin_level_thresholds[28] + 1) == 30);
    assert(dolphin_get_level(0xFFFFFFFF) == 30);

    // is_levelup fires exactly on boundaries
    assert(dolphin_state_is_levelup(300));
    assert(!dolphin_state_is_levelup(301));
    assert(dolphin_state_is_levelup(dolphin_level_thresholds[28]));

    // xp helpers
    assert(dolphin_state_xp_to_levelup(0) == 300);
    assert(dolphin_state_xp_to_levelup(0xFFFFFFFF) == 0); // capped at L30
    assert(dolphin_state_xp_above_last_levelup(0) == 0);

    printf("dolphin_level_host_test: OK (DOLPHIN_LEVEL_COUNT=%d, b[0]=%u, b[28]=%u)\n",
           DOLPHIN_LEVEL_COUNT,
           dolphin_level_thresholds[0],
           dolphin_level_thresholds[28]);
    return 0;
}
