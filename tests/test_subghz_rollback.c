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
