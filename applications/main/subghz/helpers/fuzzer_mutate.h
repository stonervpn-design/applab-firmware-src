#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FUZZ_MAX_BYTES 16    // 128-bit max frame (protocol)
#define FUZZ_MAX_DUR   256   // raw durations

typedef enum { FuzzModeRaw, FuzzModeProtocol } FuzzMode;
typedef enum {
    FuzzStrategyBitFlipSweep,
    FuzzStrategyBoundary,
    FuzzStrategyLength,
    FuzzStrategyRandom,
    FuzzStrategyFieldWalk,
} FuzzStrategy;

typedef struct {
    FuzzMode mode;
    FuzzStrategy strategy;
    uint32_t seed;
    uint8_t  seed_bits[FUZZ_MAX_BYTES];
    uint16_t seed_nbits;
    int16_t  seed_dur[FUZZ_MAX_DUR];   // +pulse / -gap
    uint16_t seed_ndur;
    uint16_t field_off, field_len;     // FieldWalk (bits)
    int16_t  len_min_delta, len_max_delta; // Length (bits or durations)
    uint8_t  rand_k;                   // Random: corruptions/frame
    uint32_t rand_iters;               // Random: frame count
} FuzzConfig;

typedef struct {
    uint8_t  bits[FUZZ_MAX_BYTES];
    uint16_t nbits;
    int16_t  dur[FUZZ_MAX_DUR];
    uint16_t ndur;
    FuzzStrategy strategy;
    uint32_t seed;
    uint32_t index;
} FuzzFrame;

// Number of frames for deterministic strategies; rand_iters for Random.
uint32_t fuzzer_mutate_count(const FuzzConfig* cfg);
// Fill out with the frame at index. false if index >= count.
bool fuzzer_mutate_frame(const FuzzConfig* cfg, uint32_t index, FuzzFrame* out);
