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
// Drive the receiver forward by D real-fob presses (original at counter `fob`).
static void drift(uint16_t fob, int D) {
    for(int i = 0; i < D; i++) rx((uint16_t)(fob + 1 + i));
}

int main(void) {
    const uint16_t C = 0x4E2; // captured counter (receiver last saw this)

    // Desync-safe step: +1 per press, no forward lead. "No fresh capture" is expressed by
    // passing the clone's CURRENT last_cnt as seen_cnt (never 0, which is a real counter).
    KeeloqClone c1 = {.last_cnt = C, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c1, c1.last_cnt) == (uint16_t)(C + 1));
    assert(keeloq_clone_next(&c1, c1.last_cnt) == (uint16_t)(C + 2));
    assert(keeloq_clone_next(&c1, c1.last_cnt) == (uint16_t)(C + 3));

    // First call catches up to a freshly-observed higher live counter, then +1;
    // seen_cnt is ignored once synced.
    KeeloqClone c2 = {.last_cnt = C, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c2, (uint16_t)(C + 50)) == (uint16_t)(C + 51));
    assert(keeloq_clone_next(&c2, c2.last_cnt) == (uint16_t)(C + 52));

    // Optional one-time forward margin (opt-in reliability lead) applied once, then +1.
    KeeloqClone c3 = {.last_cnt = C, .jump = 5, .synced = false};
    assert(keeloq_clone_next(&c3, c3.last_cnt) == (uint16_t)(C + 6));
    assert(keeloq_clone_next(&c3, c3.last_cnt) == (uint16_t)(C + 7));

    // 16-bit modular wrap is well-defined (no fresh capture: seen_cnt == last_cnt).
    KeeloqClone c4 = {.last_cnt = 0xFFFF, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c4, c4.last_cnt) == 0x0000);

    // MODULAR catch-up across the wrap: a live counter that wrapped past 0 is still "ahead"
    // of a pre-wrap last_cnt, so the clone adopts it (a plain > compare would miss this).
    KeeloqClone c5 = {.last_cnt = 0xFFF0, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c5, 0x0005) == 0x0006); // adopt 0x0005 (5 ahead of 0xFFF0), then +1

    // A behind/equal seen_cnt is ignored (here 0x0000 is behind 0x0100).
    KeeloqClone c6 = {.last_cnt = 0x0100, .jump = 0, .synced = false};
    assert(keeloq_clone_next(&c6, 0x0000) == 0x0101);

    // PROOF (unchanged): with the desync-safe +1 step, after K clone uses the ORIGINAL fob
    // recovers in exactly K+1 presses — it always recovers.
    for(int K = 1; K <= 25; K++) {
        rx_init(C);
        KeeloqClone cl = {.last_cnt = C, .jump = 0, .synced = false};
        for(int i = 0; i < K; i++) rx(keeloq_clone_next(&cl, cl.last_cnt));
        assert(recover(C) == K + 1);
    }

    // ---- FORCE-RESYNC ("Sync" button) ----

    // Margin window is (16, 0x8000): out-of-range is rejected and leaves state unchanged.
    {
        uint16_t a, b;
        KeeloqClone r = {.last_cnt = C, .synced = true};
        assert(!keeloq_clone_resync_pair(&r, r.last_cnt, 16, &a, &b));     // == WIN rejected
        assert(!keeloq_clone_resync_pair(&r, r.last_cnt, 0x8000, &a, &b)); // too large rejected
        assert(r.last_cnt == C && r.synced);                              // unchanged
        assert(keeloq_clone_resync_pair(&r, r.last_cnt, 256, &a, &b));    // valid
        assert(a == (uint16_t)(C + 256) && b == (uint16_t)(C + 257));     // consecutive pair
        assert(r.last_cnt == b && r.synced);                             // persisted to 2nd
    }

    // A single frame jumped > WIN ahead is NOT accepted alone — this is WHY Sync needs the
    // consecutive (N, N+1) pair, not just a bigger jump.
    {
        rx_init(C);
        assert(rx((uint16_t)(C + 200)) == 0); // lands in the double window: only "pending"
    }

    // PROOF: after the ORIGINAL drives the receiver forward by D presses, a Sync whose margin
    // EXCEEDS D re-locks the receiver on the pair, and an ordinary press right after still
    // works (the clone never desyncs itself). A margin <= D honestly fails to resync.
    for(int D = 0; D <= 300; D += 17) {
        // margin strictly greater than the drift -> pair accepted
        rx_init(C);
        drift(C, D); // receiver now at C+D
        KeeloqClone s = {.last_cnt = C, .synced = true};
        uint16_t f, g;
        assert(keeloq_clone_resync_pair(&s, s.last_cnt, (uint16_t)(D + 32), &f, &g));
        int a1 = rx(f), a2 = rx(g);
        assert(a1 || a2);                              // re-locked (on the 2nd frame at latest)
        assert(rx(keeloq_clone_next(&s, s.last_cnt)) == 1); // ordinary press after Sync works

        // margin <= drift cannot resync (honest boundary)
        if(D >= 20) {
            rx_init(C);
            drift(C, D);
            KeeloqClone s2 = {.last_cnt = C, .synced = true};
            uint16_t f2, g2;
            assert(keeloq_clone_resync_pair(&s2, s2.last_cnt, (uint16_t)(D - 4), &f2, &g2));
            assert(rx(f2) == 0 && rx(g2) == 0);        // both behind -> rejected
        }
    }

    printf("test_keeloq_clone_core OK (desync-safe +1, modular wrap, and force-resync pair)\n");
    return 0;
}
