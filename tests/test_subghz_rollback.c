#include "../applications/main/subghz/helpers/subghz_rollback_core.c"
#include <assert.h>
#include <stdio.h>

static RollbackSlot mk(uint32_t serial, uint32_t counter) {
    RollbackSlot s = {0};
    s.serial = serial; s.counter = counter; s.bits = 66; s.decoded = true;
    return s;
}

int main(void) {
    // --- parser: Fix/Cnt hex + MF name ---
    const char* d = "Fix:0x1ABCDEF0\r\nCnt:0034\r\nMF:Nice\r\n";
    uint32_t fix = subghz_rollback_hex_after(d, "Fix:");
    assert(fix == 0x1ABCDEF0u);
    assert((fix & 0x0FFFFFFFu) == 0x0ABCDEF0u);       // serial
    assert(((fix >> 28) & 0x0F) == 0x1);              // btn
    assert(subghz_rollback_hex_after(d, "Cnt:") == 0x34u);
    char mf[24];
    assert(subghz_rollback_manufacturer(d, mf, sizeof(mf)) && !strcmp(mf, "Nice"));
    assert(subghz_rollback_hex_after("Cnt:????", "Cnt:") == 0u); // placeholder -> 0
    assert(subghz_rollback_hex_after(d, "ZZ:") == 0u);           // missing marker

    // --- store: add, dedupe, full ---
    RollbackStore st; subghz_rollback_store_reset(&st);
    RollbackSlot a = mk(0xABCDEF0, 5), b = mk(0xABCDEF0, 6), c = mk(0xABCDEF0, 5);
    assert(subghz_rollback_store_add(&st, &a));
    assert(subghz_rollback_store_add(&st, &b));
    assert(!subghz_rollback_store_add(&st, &c));      // exact dup rejected
    assert(subghz_rollback_store_count(&st) == 2);

    // --- ready: two share a serial ---
    uint32_t serial = 0;
    assert(subghz_rollback_ready(&st, &serial) && serial == 0xABCDEF0u);
    RollbackStore st2; subghz_rollback_store_reset(&st2);
    RollbackSlot x = mk(1, 5), y = mk(2, 5);
    subghz_rollback_store_add(&st2, &x); subghz_rollback_store_add(&st2, &y);
    assert(!subghz_rollback_ready(&st2, &serial));    // different serials

    // --- target_n: default 2, clamp, accessor ---
    RollbackStore st3; subghz_rollback_store_reset(&st3);
    assert(subghz_rollback_target(&st3) == 2);         // reset default
    subghz_rollback_set_target(&st3, 3); assert(subghz_rollback_target(&st3) == 3);
    subghz_rollback_set_target(&st3, 1); assert(subghz_rollback_target(&st3) == 2);   // clamp low
    subghz_rollback_set_target(&st3, 99); assert(subghz_rollback_target(&st3) == ROLLBACK_MAX_SLOTS); // clamp high

    // --- ready_n: N=3 needs 3 CONSECUTIVE same-serial counters (Mazda CVE-2022-36945) ---
    RollbackStore m3; subghz_rollback_store_reset(&m3);
    RollbackSlot c5 = mk(0x777, 5), c6 = mk(0x777, 6), c7 = mk(0x777, 7);
    subghz_rollback_store_add(&m3, &c5); subghz_rollback_store_add(&m3, &c6);
    assert(!subghz_rollback_ready_n(&m3, 3, &serial));  // only 2 so far, N=3 -> not ready
    assert(subghz_rollback_ready_n(&m3, 2, &serial));   // ...but N=2 IS ready (pair)
    subghz_rollback_store_add(&m3, &c7);
    assert(subghz_rollback_ready_n(&m3, 3, &serial) && serial == 0x777u); // 5,6,7 -> ready

    // gapped: 5,6,50 same serial -> N=3 NOT ready (cannot resync), but N=2 ready
    RollbackStore gap; subghz_rollback_store_reset(&gap);
    RollbackSlot g5 = mk(0x888, 5), g6 = mk(0x888, 6), g50 = mk(0x888, 50);
    subghz_rollback_store_add(&gap, &g5); subghz_rollback_store_add(&gap, &g6);
    subghz_rollback_store_add(&gap, &g50);
    assert(!subghz_rollback_ready_n(&gap, 3, &serial)); // gap after 6 -> no run of 3
    assert(subghz_rollback_ready_n(&gap, 2, &serial));  // 5,6 pair still fine at N=2

    // constant step != 1 (fob increments by 5): 5,10,15 -> N=3 ready (consecutive presses)
    RollbackStore step5; subghz_rollback_store_reset(&step5);
    RollbackSlot s5 = mk(0x999, 5), s10 = mk(0x999, 10), s15 = mk(0x999, 15);
    subghz_rollback_store_add(&step5, &s5); subghz_rollback_store_add(&step5, &s10);
    subghz_rollback_store_add(&step5, &s15);
    assert(subghz_rollback_ready_n(&step5, 3, &serial) && serial == 0x999u);

    // --- ordering: added 5 then 6 ---
    uint8_t idx[ROLLBACK_MAX_SLOTS];
    uint8_t n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderAsc, idx, ROLLBACK_MAX_SLOTS);
    assert(n == 2 && st.slot[idx[0]].counter == 5 && st.slot[idx[1]].counter == 6);
    n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderDesc, idx, ROLLBACK_MAX_SLOTS);
    assert(n == 2 && st.slot[idx[0]].counter == 6 && st.slot[idx[1]].counter == 5);
    n = subghz_rollback_order_indices(&st, 0xABCDEF0, RollbackOrderAsCaptured, idx, ROLLBACK_MAX_SLOTS);
    assert(n == 2 && idx[0] == 0 && idx[1] == 1);

    printf("test_subghz_rollback OK\n");
    return 0;
}
