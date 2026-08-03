#include "../lib/subghz/protocols/subghz_keeloq_clone_core.c"
#include <assert.h>
#include <stdio.h>

static KeeloqClone mk(uint16_t last, uint16_t jump, bool synced) {
    KeeloqClone c = {0};
    c.serial = 0x0ABCDEF; c.btn = 2; c.kdev = 0x1122334455667788ull;
    c.learn_type = 2; c.last_cnt = last; c.jump = jump; c.synced = synced;
    return c;
}

int main(void) {
    uint16_t a = 0, b = 0;

    // Fresh clone: one-time jump lead, then synced.
    KeeloqClone c1 = mk(5, 100, false);
    keeloq_clone_next_pair(&c1, 0, &a, &b);
    assert(a == 105 && b == 106 && c1.last_cnt == 106 && c1.synced == true);

    // Subsequent send: +2 pair advance (no re-jump).
    keeloq_clone_next_pair(&c1, 0, &a, &b);
    assert(a == 107 && b == 108 && c1.last_cnt == 108);

    // Fresh capture ahead of stored last -> catch up before jump.
    KeeloqClone c2 = mk(5, 100, false);
    keeloq_clone_next_pair(&c2, 500, &a, &b);
    assert(a == 600 && b == 601 && c2.last_cnt == 601 && c2.synced == true);

    // 16-bit wrap on the +1 pair.
    KeeloqClone c3 = mk(0xFFFF, 100, true);
    keeloq_clone_next_pair(&c3, 0, &a, &b);
    assert(a == 0x0000 && b == 0x0001 && c3.last_cnt == 0x0001);

    // Jump that overflows 16 bits wraps modularly.
    KeeloqClone c4 = mk(0xFFF0, 0x100, false);
    keeloq_clone_next_pair(&c4, 0, &a, &b);
    assert(a == 0x00F0 && b == 0x00F1);

    // Synced branch IGNORES seen_cnt even when it is far ahead (regression for the fix).
    KeeloqClone c5 = mk(50, 100, true);
    keeloq_clone_next_pair(&c5, 1000, &a, &b);
    assert(a == 51 && b == 52 && c5.last_cnt == 52);

    // out_b-only wrap: target itself does not wrap, target+1 does.
    KeeloqClone c6 = mk(0xFFFE, 100, true);
    keeloq_clone_next_pair(&c6, 0, &a, &b);
    assert(a == 0xFFFF && b == 0x0000 && c6.last_cnt == 0x0000);

    printf("test_keeloq_clone_core OK\n");
    return 0;
}
