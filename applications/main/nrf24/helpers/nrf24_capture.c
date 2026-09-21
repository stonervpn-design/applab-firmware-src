#include "nrf24_capture.h"

#include <string.h>
#include <stdlib.h>

/* Fixed-capacity ring: g_head is the logical index (mod N24_CAP_MAX) of the
 * oldest stored entry; g_count is how many entries are valid (saturates at
 * N24_CAP_MAX). nrf24_capture_get(i) maps i -> physical (g_head + i) % MAX,
 * so index 0 is always oldest regardless of wrap point.
 *
 * RAM: the N24_CAP_MAX-entry ring (~3 KB) is allocated ON DEMAND on the first
 * add, not resident at boot -- it only matters while the (private-only) nRF24
 * attack scenes are open. nrf24_capture_free() (called from nrf24_app_free
 * after all scenes/workers have stopped) returns it to the heap. */
static Nrf24Capture* g_caps;
static size_t g_head;
static size_t g_count;

void nrf24_capture_reset(void) {
    g_head = 0;
    g_count = 0;
}

bool nrf24_capture_add(const Nrf24Capture* c) {
    if(c == NULL) return false;

    if(g_caps == NULL) {
        g_caps = malloc(sizeof(*g_caps) * N24_CAP_MAX);
        if(g_caps == NULL) return false; /* out of heap: drop the frame */
        g_head = 0;
        g_count = 0;
    }

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
    if(g_caps == NULL || i >= g_count) return NULL;
    return &g_caps[(g_head + i) % N24_CAP_MAX];
}

void nrf24_capture_free(void) {
    free(g_caps);
    g_caps = NULL;
    g_head = 0;
    g_count = 0;
}
