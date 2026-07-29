#include "subbrute_debruijn.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Greedy "prefer-ones" (Martin's algorithm) de Bruijn B(2,n) construction:
 * start from the all-zeros n-bit window, then at each step append a 1 if the
 * resulting window has not been visited, otherwise append a 0. After 2^n steps
 * every n-bit window has been visited exactly once; we then append the first
 * (n-1) bits so the linear sequence also covers the wrap-around windows.
 */

size_t subbrute_debruijn_length(uint8_t n) {
    if(n < SUBBRUTE_DEBRUIJN_MIN_N || n > SUBBRUTE_DEBRUIJN_MAX_N) {
        return 0;
    }
    return ((size_t)1u << n) + n - 1u;
}

uint8_t* subbrute_debruijn_generate(uint8_t n, size_t* out_len) {
    if(out_len) {
        *out_len = 0;
    }
    if(n < SUBBRUTE_DEBRUIJN_MIN_N || n > SUBBRUTE_DEBRUIJN_MAX_N) {
        return NULL;
    }

    const uint32_t total = 1u << n; // number of distinct n-bit windows / cyclic length
    const uint32_t mask = total - 1u;
    const size_t seq_len = (size_t)total + n - 1u;

    uint8_t* seq = malloc(seq_len);
    uint8_t* visited = calloc(total, sizeof(uint8_t)); // one flag per window value
    if(!seq || !visited) {
        free(seq);
        free(visited);
        return NULL;
    }

    uint32_t window = 0u; // current n-bit window, starts all-zeros
    visited[window] = 1u; // the all-zeros window is the starting state
    for(uint32_t i = 0; i < total; i++) {
        // Prefer appending 1: candidate window shifts in a 1.
        uint32_t next_one = ((window << 1) | 1u) & mask;
        uint8_t bit;
        if(!visited[next_one]) {
            bit = 1u;
            window = next_one;
        } else {
            bit = 0u;
            window = (window << 1) & mask;
        }
        visited[window] = 1u;
        seq[i] = bit;
    }

    // Append the first (n-1) bits so linear windows cover the cyclic wrap-around.
    for(uint32_t i = 0; i < (uint32_t)(n - 1u); i++) {
        seq[total + i] = seq[i];
    }

    free(visited);
    if(out_len) {
        *out_len = seq_len;
    }
    return seq;
}

char* subbrute_debruijn_raw_data(const uint8_t* seq, size_t len, uint32_t te) {
    if(!seq || len == 0 || te == 0 || seq[0] != 1u) {
        return NULL; // must start carrier-on for a valid Flipper RAW_Data
    }

    // Worst case one run per bit; each duration prints as at most "-NNNNNNNNNN "
    // (uint32 microseconds, sign, space) = 12 chars. Allocate generously + NUL.
    size_t cap = len * 13u + 1u;
    char* out = malloc(cap);
    if(!out) {
        return NULL;
    }

    size_t pos = 0;
    size_t i = 0;
    while(i < len) {
        uint8_t bit = seq[i] & 1u;
        size_t run = 1;
        while(i + run < len && (seq[i + run] & 1u) == bit) {
            run++;
        }
        // Cap run length so run*te can't overflow a 32-bit duration.
        uint64_t dur = (uint64_t)run * (uint64_t)te;
        long signed_dur = bit ? (long)dur : -(long)dur;
        int wrote = snprintf(
            out + pos, cap - pos, (pos == 0) ? "%ld" : " %ld", signed_dur);
        if(wrote < 0 || (size_t)wrote >= cap - pos) {
            free(out);
            return NULL;
        }
        pos += (size_t)wrote;
        i += run;
    }
    out[pos] = '\0';
    return out;
}
