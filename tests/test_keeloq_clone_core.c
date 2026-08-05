#include "../lib/subghz/protocols/subghz_keeloq_clone_core.c"
#include <assert.h>
#include <stdio.h>

// Minimal KeeLoq receiver model. The receiver stores ONE counter per serial; a clone
// shares the original fob's serial, so both hit the same slot. Accept rule (HCS-style):
//   1..WIN ahead  -> single operating window: accept, resync
//   WIN+1..32768  -> double window: accept only as the 2nd of two consecutive counters
//   0 or behind   -> reject (replay / stale)
#define WIN 16
static uint16_t rstored;
static int rpending;
static uint16_t rpend;
static void rx_init(uint16_t s) {
    rstored = s;
    rpending = 0;
}
static int rx(uint16_t c) {
    uint16_t d = (uint16_t)(c - rstored);
    if(d >= 1 && d <= WIN) {
        rstored = c;
        rpending = 0;
        return 1;
    }
    if(d > WIN && d <= 32768) {
        if(rpending && c == (uint16_t)(rpend + 1)) {
            rstored = c;
            rpending = 0;
            return 1;
        }
        rpending = 1;
        rpend = c;
        return 0;
    }
    return 0; // d == 0 (replay) or behind
}
// The original fob presses (counter +1 each) until the receiver accepts; #presses.
static int recover(uint16_t fob) {
    uint16_t c = fob;
    for(int i = 1; i <= 5000; i++) {
        c++;
        if(rx(c)) return i;
    }
    return 9999;
}

int main(void) {
    const uint16_t C = 0x4E2; // captured counter (receiver last saw this)

    // Desync-safe step: +1 per press, no forward lead.
    KeeloqClone c1 = {.last_cnt = C, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c1, 0) == (uint16_t)(C + 1));
    assert(keeloq_clone_next(&c1, 0) == (uint16_t)(C + 2));
    assert(keeloq_clone_next(&c1, 0) == (uint16_t)(C + 3));

    // First call catches up to a freshly-observed higher live counter, then +1;
    // seen_cnt is ignored once synced.
    KeeloqClone c2 = {.last_cnt = C, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c2, (uint16_t)(C + 50)) == (uint16_t)(C + 51));
    assert(keeloq_clone_next(&c2, 0) == (uint16_t)(C + 52));

    // Optional one-time forward margin (opt-in reliability lead) applied once, then +1.
    KeeloqClone c3 = {.last_cnt = C, .jump = 5, .synced = false};
    assert(keeloq_clone_next(&c3, 0) == (uint16_t)(C + 6));
    assert(keeloq_clone_next(&c3, 0) == (uint16_t)(C + 7));

    // 16-bit modular wrap is well-defined.
    KeeloqClone c4 = {.last_cnt = 0xFFFF, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c4, 0) == 0x0000);

    // PROOF: with the desync-safe step, after K clone uses the ORIGINAL fob recovers in
    // exactly K+1 presses — never the old fixed ~100-press wall, and it always recovers.
    for(int K = 1; K <= 25; K++) {
        rx_init(C);
        KeeloqClone cl = {.last_cnt = C, .jump = 0, .synced = false};
        for(int i = 0; i < K; i++) rx(keeloq_clone_next(&cl, 0));
        assert(recover(C) == K + 1);
    }

    printf("test_keeloq_clone_core OK (desync-safe: original recovers in K+1 presses)\n");
    return 0;
}
