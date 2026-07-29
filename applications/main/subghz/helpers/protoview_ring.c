#include "protoview_ring.h"
#include <furi.h>
#include <stdlib.h>
#include <string.h>

// No-PSRAM RAM diet: the capture ring is allocated only while the ProtoView
// scene is active (pv_ring_alloc on enter, pv_ring_free on exit) instead of
// sitting in .bss permanently. Idle cost is now 0 instead of 32 KB, and the
// second "s_tmp" rotation buffer is gone — the snapshot rotates in place.
static PvSample* s_ring;             // PV_RING_LEN samples, NULL when scene inactive
static size_t s_head;                // next write index
static size_t s_count;               // valid samples
static bool s_frozen;
// push() runs on the SubGhz RX worker task; snapshot() rotates the ring in place
// on the GUI task. Without this the rotation + s_head reset races the writer,
// corrupting samples and clobbering s_head. Contention window is a bounded O(n)
// rotation (~tens of µs), so the writer is never blocked meaningfully.
static FuriMutex* s_mux;

bool pv_ring_alloc(void) {
    if(!s_ring) {
        s_ring = malloc(sizeof(PvSample) * PV_RING_LEN);
    }
    if(!s_mux) {
        s_mux = furi_mutex_alloc(FuriMutexTypeNormal);
    }
    s_head = 0;
    s_count = 0;
    s_frozen = false;
    return s_ring != NULL && s_mux != NULL;
}

void pv_ring_free(void) {
    if(s_ring) {
        free(s_ring);
        s_ring = NULL;
    }
    if(s_mux) {
        furi_mutex_free(s_mux);
        s_mux = NULL;
    }
    s_head = 0;
    s_count = 0;
    s_frozen = false;
}

void pv_ring_reset(void) {
    s_head = 0;
    s_count = 0;
    s_frozen = false;
}

void pv_ring_push(bool level, uint32_t duration) {
    if(!s_ring || !s_mux || s_frozen) return;
    furi_mutex_acquire(s_mux, FuriWaitForever);
    s_ring[s_head].dur = duration;
    s_ring[s_head].level = level ? 1 : 0;
    s_head = (s_head + 1) % PV_RING_LEN;
    if(s_count < PV_RING_LEN) s_count++;
    furi_mutex_release(s_mux);
}

void pv_ring_freeze(bool frozen) {
    s_frozen = frozen;
}

bool pv_ring_is_frozen(void) {
    return s_frozen;
}

size_t pv_ring_count(void) {
    return s_count;
}

// Reverse s_ring[a..b) in place.
static void pv_reverse(size_t a, size_t b) {
    while(a < b) {
        b--;
        PvSample t = s_ring[a];
        s_ring[a] = s_ring[b];
        s_ring[b] = t;
        a++;
    }
}

size_t pv_ring_snapshot(const PvSample** out) {
    if(!s_ring || !s_mux) {
        *out = NULL;
        return 0;
    }
    furi_mutex_acquire(s_mux, FuriWaitForever);
    // Rotate so the oldest sample sits at index 0. In-place left-rotation by
    // s_head positions (three-reversal algorithm) — no second buffer needed.
    // Held under the mutex so a concurrent push() can't tear the rotation or
    // observe the transient s_head=0 reset.
    if(s_count == PV_RING_LEN && s_head != 0) {
        size_t k = s_head; // oldest sample index == left-rotate amount
        pv_reverse(0, k);
        pv_reverse(k, PV_RING_LEN);
        pv_reverse(0, PV_RING_LEN);
        s_head = 0;
    }
    // When not full, samples already occupy [0, s_count) in order.
    size_t n = s_count;
    furi_mutex_release(s_mux);
    *out = s_ring;
    return n;
}
