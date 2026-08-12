#include "../applications/main/subghz/helpers/subghz_fob_nav.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    // grid: 0 1 / 2 3
    assert(subghz_fob_nav_move(0, FOB_RIGHT) == 1);
    assert(subghz_fob_nav_move(1, FOB_RIGHT) == 0); // wrap row
    assert(subghz_fob_nav_move(0, FOB_LEFT)  == 1); // wrap row
    assert(subghz_fob_nav_move(0, FOB_DOWN)  == 2);
    assert(subghz_fob_nav_move(2, FOB_DOWN)  == 0); // wrap col
    assert(subghz_fob_nav_move(3, FOB_UP)    == 1);
    assert(subghz_fob_nav_move(0, FOB_CYCLE) == 1); // cycle order 0->1->2->3->0
    assert(subghz_fob_nav_move(3, FOB_CYCLE) == 0);
    assert(subghz_fob_nav_move(2, FOB_CYCLE) == 3);

    // _move_n(n=4) must be identical to the legacy 2x2 wrapper.
    for(uint8_t i = 0; i < 4; i++)
        for(uint8_t k = FOB_LEFT; k <= FOB_CYCLE; k++)
            assert(subghz_fob_nav_move_n(i, k, 4) == subghz_fob_nav_move(i, k));

    // n=3 (VAG): linear stepping mod 3, result always < 3.
    assert(subghz_fob_nav_move_n(0, FOB_CYCLE, 3) == 1); // 0->1->2->0
    assert(subghz_fob_nav_move_n(1, FOB_CYCLE, 3) == 2);
    assert(subghz_fob_nav_move_n(2, FOB_CYCLE, 3) == 0);
    assert(subghz_fob_nav_move_n(2, FOB_RIGHT, 3) == 0); // next wraps
    assert(subghz_fob_nav_move_n(0, FOB_DOWN, 3) == 1);  // next
    assert(subghz_fob_nav_move_n(0, FOB_LEFT, 3) == 2);  // prev wraps 0->2
    assert(subghz_fob_nav_move_n(0, FOB_UP, 3) == 2);    // prev wraps
    assert(subghz_fob_nav_move_n(1, FOB_LEFT, 3) == 0);  // prev
    assert(subghz_fob_nav_move_n(5, FOB_CYCLE, 3) < 3);  // out-of-range idx clamps
    assert(subghz_fob_nav_move_n(0, FOB_CYCLE, 0) == 0); // n=0 guard

    // n=6 (Honda 6-tile): 2 columns, 3 rows -> 0 1 / 2 3 / 4 5.
    assert(subghz_fob_nav_move_n(0, FOB_RIGHT, 6) == 1);
    assert(subghz_fob_nav_move_n(0, FOB_DOWN, 6) == 2);
    assert(subghz_fob_nav_move_n(2, FOB_DOWN, 6) == 4);
    assert(subghz_fob_nav_move_n(4, FOB_DOWN, 6) == 0); // wrap to top row
    assert(subghz_fob_nav_move_n(0, FOB_UP, 6) == 4);   // wrap to bottom row
    assert(subghz_fob_nav_move_n(5, FOB_LEFT, 6) == 4);
    // cycle visits every tile exactly once then wraps.
    {
        uint8_t seen = 0, cur = 0;
        for(int step = 0; step < 6; step++) {
            assert(cur < 6);
            seen |= (uint8_t)(1u << cur);
            cur = subghz_fob_nav_move_n(cur, FOB_CYCLE, 6);
        }
        assert(seen == 0x3F); // all 6 tiles reached
        assert(cur == 0);     // wrapped back to start
    }

    // n=5 (short last row: 0 1 / 2 3 / 4): moves never leave [0,5).
    for(uint8_t i = 0; i < 5; i++)
        for(uint8_t k = FOB_LEFT; k <= FOB_CYCLE; k++)
            assert(subghz_fob_nav_move_n(i, k, 5) < 5);
    assert(subghz_fob_nav_move_n(4, FOB_RIGHT, 5) == 4); // short row: no right neighbor
    assert(subghz_fob_nav_move_n(3, FOB_DOWN, 5) == 4);  // col1 down -> clamps into tile 4

    // Every tile of an 8-tile table is reachable and in-range under all keys.
    for(uint8_t i = 0; i < 8; i++)
        for(uint8_t k = FOB_LEFT; k <= FOB_CYCLE; k++)
            assert(subghz_fob_nav_move_n(i, k, 8) < 8);

    printf("test_fob_nav OK\n");
    return 0;
}
