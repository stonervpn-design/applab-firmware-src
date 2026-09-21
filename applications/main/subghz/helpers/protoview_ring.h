#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PV_RING_LEN 2048

typedef struct {
    uint32_t dur;
    uint8_t level;
} PvSample;

// Allocate/free the capture ring. The buffer lives only while the ProtoView
// scene is active, so idle heap is not consumed. pv_ring_alloc returns false if
// the (single PV_RING_LEN) buffer could not be allocated on the tight heap;
// callers degrade gracefully (push is a no-op, snapshot returns 0).
bool pv_ring_alloc(void);
void pv_ring_free(void);

void pv_ring_reset(void);
void pv_ring_push(bool level, uint32_t duration);
void pv_ring_freeze(bool frozen);
bool pv_ring_is_frozen(void);
size_t pv_ring_count(void);
size_t pv_ring_snapshot(const PvSample** out);
