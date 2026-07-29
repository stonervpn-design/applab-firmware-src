#include "../applications/main/subghz/helpers/protoview_ring.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    pv_ring_reset();
    assert(pv_ring_count() == 0 && !pv_ring_is_frozen());

    for (uint32_t i = 0; i < 100; i++) pv_ring_push(i & 1, i + 1);
    assert(pv_ring_count() == 100);
    const PvSample* w = NULL;
    assert(pv_ring_snapshot(&w) == 100);
    assert(w[0].dur == 1 && w[99].dur == 100);  // oldest-first order

    // overflow: push past capacity, oldest overwritten
    pv_ring_reset();
    for (uint32_t i = 0; i < PV_RING_LEN + 500; i++) pv_ring_push(i & 1, i + 1);
    assert(pv_ring_count() == PV_RING_LEN);
    assert(pv_ring_snapshot(&w) == PV_RING_LEN);
    assert(w[0].dur == 501);  // first 500 dropped
    assert(w[PV_RING_LEN - 1].dur == PV_RING_LEN + 500);

    // freeze blocks writes
    pv_ring_freeze(true);
    pv_ring_push(1, 99999);
    assert(pv_ring_snapshot(&w) == PV_RING_LEN && w[PV_RING_LEN - 1].dur == PV_RING_LEN + 500);
    pv_ring_freeze(false);
    printf("test_protoview_ring OK\n");
    return 0;
}
