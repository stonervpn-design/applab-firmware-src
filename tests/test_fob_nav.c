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

    printf("test_fob_nav OK\n");
    return 0;
}
