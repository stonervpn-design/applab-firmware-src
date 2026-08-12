// tests/test_nrf24_capture.c
// Host unit test for the nRF24 capture engine (Task 1 of the nRF24
// Red-Team Suite). Compile+run under WSL:
//   wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" &&
//     gcc -std=gnu11 -Wall -Wextra -Iapplications/main/nrf24/helpers
//     tests/test_nrf24_capture.c applications/main/nrf24/helpers/nrf24_capture.c
//     -o /tmp/tnc && /tmp/tnc'

#include "nrf24_capture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static Nrf24Capture make_cap(uint8_t channel) {
    Nrf24Capture c;
    memset(&c, 0, sizeof c);
    c.addr[0] = channel; /* distinguishing field, mirrors channel */
    c.channel = channel;
    c.payload[0] = 0xAA;
    c.len = 1;
    c.ts = 1000u + channel;
    c.proto = N24_RAW;
    return c;
}

static void test_reset_is_empty(void) {
    nrf24_capture_reset();
    assert(nrf24_capture_count() == 0);
    assert(nrf24_capture_get(0) == NULL);
    printf("test_reset_is_empty OK\n");
}

static void test_add_under_capacity_preserves_order(void) {
    nrf24_capture_reset();
    Nrf24Capture a = make_cap(10);
    Nrf24Capture b = make_cap(20);
    Nrf24Capture c = make_cap(30);
    assert(nrf24_capture_add(&a));
    assert(nrf24_capture_add(&b));
    assert(nrf24_capture_add(&c));
    assert(nrf24_capture_count() == 3);

    const Nrf24Capture* g0 = nrf24_capture_get(0);
    const Nrf24Capture* g1 = nrf24_capture_get(1);
    const Nrf24Capture* g2 = nrf24_capture_get(2);
    assert(g0 != NULL && g0->channel == 10);
    assert(g1 != NULL && g1->channel == 20);
    assert(g2 != NULL && g2->channel == 30);
    /* out of range */
    assert(nrf24_capture_get(3) == NULL);
    printf("test_add_under_capacity_preserves_order OK\n");
}

static void test_overflow_drops_oldest_ring(void) {
    nrf24_capture_reset();
    const size_t total = N24_CAP_MAX + 5; /* channels 0..MAX+4 */
    for (size_t i = 0; i < total; i++) {
        Nrf24Capture c = make_cap((uint8_t)(i & 0xFF));
        assert(nrf24_capture_add(&c));
    }
    assert(nrf24_capture_count() == N24_CAP_MAX);

    /* oldest 5 (channels 0..4) dropped; get(0) is the 6th inserted (channel 5) */
    const Nrf24Capture* g0 = nrf24_capture_get(0);
    assert(g0 != NULL && g0->channel == 5);

    /* get(MAX-1) is the very last inserted: channel (total-1) & 0xFF */
    const Nrf24Capture* glast = nrf24_capture_get(N24_CAP_MAX - 1);
    assert(glast != NULL && glast->channel == (uint8_t)((total - 1) & 0xFF));

    /* still saturates, doesn't grow past MAX */
    assert(nrf24_capture_get(N24_CAP_MAX) == NULL);
    printf("test_overflow_drops_oldest_ring OK\n");
}

static void test_reset_again_clears(void) {
    nrf24_capture_reset();
    assert(nrf24_capture_count() == 0);
    assert(nrf24_capture_get(0) == NULL);
    printf("test_reset_again_clears OK\n");
}

int main(void) {
    test_reset_is_empty();
    test_add_under_capacity_preserves_order();
    test_overflow_drops_oldest_ring();
    test_reset_again_clears();
    printf("test_nrf24_capture: ALL PASS\n");
    return 0;
}
