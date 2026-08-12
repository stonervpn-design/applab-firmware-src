#include "nrf24_capture.h"

#include <string.h>

/* Fixed-capacity ring: g_head is the logical index (mod N24_CAP_MAX) of the
 * oldest stored entry; g_count is how many entries are valid (saturates at
 * N24_CAP_MAX). nrf24_capture_get(i) maps i -> physical (g_head + i) % MAX,
 * so index 0 is always oldest regardless of wrap point. */
static Nrf24Capture g_caps[N24_CAP_MAX];
static size_t g_head;
static size_t g_count;

void nrf24_capture_reset(void) {
    g_head = 0;
    g_count = 0;
}

bool nrf24_capture_add(const Nrf24Capture* c) {
    if(c == NULL) return false;

    size_t write_idx = (g_head + g_count) % N24_CAP_MAX;
    g_caps[write_idx] = *c;

    if(g_count < N24_CAP_MAX) {
        g_count++;
    } else {
        /* already at capacity: the write above just overwrote the oldest
         * slot, so the new oldest is one further along */
        g_head = (g_head + 1) % N24_CAP_MAX;
    }
    return true;
}

size_t nrf24_capture_count(void) {
    return g_count;
}

const Nrf24Capture* nrf24_capture_get(size_t i) {
    if(i >= g_count) return NULL;
    return &g_caps[(g_head + i) % N24_CAP_MAX];
}
