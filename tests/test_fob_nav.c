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
    printf("test_fob_nav OK\n");
    return 0;
}
