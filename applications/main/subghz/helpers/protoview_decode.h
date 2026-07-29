#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protoview_ring.h"   // PvSample

typedef struct {
    uint32_t t_short, t_long, t_sync;
} PvThresholds;

// Raw output kept furi-free (no LevelDuration) for host-testability; the scene
// converts PvRawSymbol -> signed RAW_Data ints when it writes the .sub.
typedef struct {
    uint32_t dur;
    uint8_t level;
} PvRawSymbol;

// Histogram of durations -> short/long thresholds; t_sync = longest gap seen.
// Returns false if fewer than 8 edges or no clear short/long split.
bool pv_classify_symbols(const PvSample* s, size_t n, PvThresholds* out);

// PWM / PT2262: short-high+long-low -> '0', long-high+short-low -> '1'.
// Returns bit count (0 if nothing decodes). Writes a NUL-terminated bitstring.
int pv_decode_pwm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);

// Manchester (IEEE): mid-bit low->high = '1', high->low = '0'. Returns bit count, <0 on desync.
int pv_decode_manchester(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);

// PPM / Oregon-style: fixed pulse, gap==t_short -> '0', gap==t_long -> '1'. Returns bit count.
int pv_decode_ppm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap);

// Bitstring -> raw symbols at te microseconds per short element (PWM encoding). Returns symbol count.
size_t pv_bits_to_raw(const char* bits, uint32_t te, PvRawSymbol* out, size_t cap);
