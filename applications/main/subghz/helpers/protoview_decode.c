#include "protoview_decode.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

// Helper: classify a duration as short, long, or neither
// Returns: 0 = short, 1 = long, -1 = neither (e.g., sync gap)
static int classify_duration(uint32_t dur, uint32_t t_short, uint32_t t_long) {
    // Distance to nearest threshold
    uint32_t dist_short = (dur > t_short) ? (dur - t_short) : (t_short - dur);
    uint32_t dist_long = (dur > t_long) ? (dur - t_long) : (t_long - dur);

    // Use a tolerance of 50% of t_short
    uint32_t tol = t_short / 2;

    if (dist_short <= tol && dist_short < dist_long) {
        return 0;  // Short
    } else if (dist_long <= tol && dist_long < dist_short) {
        return 1;  // Long
    }
    return -1;  // Doesn't match either
}

// Half-octave log bucket: floor(2 * log2(d)). Groups durations by ratio, not by
// absolute value, so a wide dynamic range (short pulse vs long sync gap) is
// represented with fixed resolution.
static uint32_t pv_log2_half(uint32_t d) {
    if (d < 2) return 0;
    uint32_t l = 31u - (uint32_t)__builtin_clz(d);            // floor(log2(d))
    // second half-octave once d passes ~sqrt(2)*2^l (approximated by 1.5*2^l)
    uint32_t half = (d >= (((uint32_t)1 << l) + ((uint32_t)1 << (l - 1)))) ? 1u : 0u;
    return 2u * l + half;
}

#define PV_LOG_BINS 32

bool pv_classify_symbols(const PvSample* s, size_t n, PvThresholds* out) {
    if (n < 8) return false;

    // Log-domain histogram. OOK durations span a wide range — data pulses are
    // hundreds of us, but the inter-frame / sync gap is tens of ms. A LINEAR
    // histogram over min..max makes each bin milliseconds wide, so short AND long
    // data pulses collapse into a single bin and there is no distinct second peak
    // ("no clear symbols"). Half-octave log buckets keep short, long and sync in
    // separate bins whatever the absolute scale.
    uint32_t hist[PV_LOG_BINS] = {0};
    uint32_t bin_sum[PV_LOG_BINS] = {0};  // running sum per bin, for centroids
    uint32_t max_dur = 0;

    for (size_t i = 0; i < n; ++i) {
        uint32_t d = s[i].dur;
        if (d > max_dur) max_dur = d;
        uint32_t b = pv_log2_half(d);
        if (b >= PV_LOG_BINS) b = PV_LOG_BINS - 1;
        hist[b]++;
        bin_sum[b] += d;
    }

    // The two MOST-POPULATED buckets are the short and long data-pulse clusters
    // (each makes up a large share of the frame). The sync gap is sparse (one per
    // repeat) and sits in a much higher bucket, so it is naturally excluded.
    int peak1 = -1, peak2 = -1;
    uint32_t c1 = 0, c2 = 0;
    for (int i = 0; i < PV_LOG_BINS; ++i) {
        if (hist[i] > c1) {
            c2 = c1; peak2 = peak1;
            c1 = hist[i]; peak1 = i;
        } else if (hist[i] > c2) {
            c2 = hist[i]; peak2 = i;
        }
    }

    if (peak1 < 0 || peak2 < 0) return false;
    if (c1 < 2 || c2 < 2) return false;

    uint32_t centroid1 = bin_sum[peak1] / hist[peak1];
    uint32_t centroid2 = bin_sum[peak2] / hist[peak2];

    uint32_t t_short = (centroid1 < centroid2) ? centroid1 : centroid2;
    uint32_t t_long  = (centroid1 > centroid2) ? centroid1 : centroid2;

    if (t_short < 1) return false;
    // Require a real short/long separation (>= ~1.5x). Manchester ~2x, PWM ~3x;
    // adjacent half-octave jitter (~1.41x) is rejected as a single-width signal.
    if (t_long < (t_short + t_short / 2)) return false;

    out->t_short = t_short;
    out->t_long = t_long;
    out->t_sync = max_dur;

    return true;
}

int pv_decode_pwm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap) {
    if (n < 2 || cap < 2) return 0;

    int bit_count = 0;
    size_t i = 0;

    // Skip leading sync gap if present
    if (i < n && s[i].dur > th->t_long + th->t_long / 2) {
        i++;
        if (i >= n) return 0;
    }

    // Process sample pairs (high then low)
    while (i + 1 < n && bit_count < (int)cap - 1) {
        uint32_t high_dur = s[i].dur;
        uint32_t low_dur = s[i + 1].dur;

        int high_class = classify_duration(high_dur, th->t_short, th->t_long);
        int low_class = classify_duration(low_dur, th->t_short, th->t_long);

        // Stop if we hit sync gap at the end
        if (high_dur > th->t_long + th->t_long / 2) break;

        if (high_class >= 0 && low_class >= 0) {
            // H_short+L_long -> '0', H_long+L_short -> '1'
            char bit = (high_class == 0 && low_class == 1) ? '0' :
                       (high_class == 1 && low_class == 0) ? '1' : '?';

            if (bit != '?') {
                bits[bit_count++] = bit;
            }
        }

        i += 2;
    }

    bits[bit_count] = '\0';
    return bit_count;
}

int pv_decode_manchester(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap) {
    if (n < 2 || cap < 2) return 0;

    int bit_count = 0;

    // Manchester: track time within a bit cell (2 half-bits)
    // mid-bit low->high = '1', high->low = '0'
    uint32_t half_bit = th->t_short;
    uint32_t tol = half_bit / 2;

    uint32_t time_in_cell = 0;
    int last_level = -1;
    bool had_mid_transition = false;
    int mid_bit_edge = -1;  // -1: none, 0: high->low, 1: low->high

    for (size_t i = 0; i < n && bit_count < (int)cap - 1; ++i) {
        uint32_t dur = s[i].dur;
        int level = s[i].level;

        // Check if duration is roughly a half-bit
        if (dur < half_bit - tol || dur > half_bit + tol) {
            continue;  // Out of sync, skip
        }

        // Detect edge type (only care at mid-bit)
        if (last_level >= 0 && last_level != level) {
            // Edge detected
            if (time_in_cell >= half_bit - tol && time_in_cell <= half_bit + tol) {
                // This is a mid-bit edge
                had_mid_transition = true;
                if (last_level == 1) {
                    mid_bit_edge = 0;  // high->low
                } else {
                    mid_bit_edge = 1;  // low->high
                }
            }
            time_in_cell = 0;
        } else {
            time_in_cell += dur;
        }

        // Emit bit when we complete a cell
        if (time_in_cell >= 2 * half_bit - tol) {
            if (had_mid_transition) {
                bits[bit_count++] = (mid_bit_edge == 1) ? '1' : '0';
            }
            time_in_cell = 0;
            had_mid_transition = false;
            mid_bit_edge = -1;
        }

        last_level = level;
    }

    bits[bit_count] = '\0';
    return bit_count;
}

int pv_decode_ppm(const PvSample* s, size_t n, const PvThresholds* th, char* bits, size_t cap) {
    if (n < 2 || cap < 2) return 0;

    int bit_count = 0;

    // PPM: process pairs (high pulse, gap/duration)
    // Assume roughly constant high pulse, classify the gap
    for (size_t i = 0; i + 1 < n && bit_count < (int)cap - 1; i += 2) {
        uint32_t pulse_dur = s[i].dur;
        uint32_t gap_dur = s[i + 1].dur;

        // Skip if pulse is too long (sync marker)
        if (pulse_dur > th->t_long + th->t_long / 2) {
            continue;
        }

        int gap_class = classify_duration(gap_dur, th->t_short, th->t_long);

        if (gap_class == 0) {
            bits[bit_count++] = '0';  // short gap
        } else if (gap_class == 1) {
            bits[bit_count++] = '1';  // long gap
        }
    }

    bits[bit_count] = '\0';
    return bit_count;
}

size_t pv_bits_to_raw(const char* bits, uint32_t te, PvRawSymbol* out, size_t cap) {
    if (!bits || cap < 2) return 0;

    size_t idx = 0;

    for (const char* p = bits; *p && idx + 1 < cap; ++p) {
        if (*p == '1') {
            // '1': long-high + short-low
            out[idx].level = 1;
            out[idx].dur = te * 3;
            idx++;
            out[idx].level = 0;
            out[idx].dur = te;
            idx++;
        } else if (*p == '0') {
            // '0': short-high + long-low
            out[idx].level = 1;
            out[idx].dur = te;
            idx++;
            out[idx].level = 0;
            out[idx].dur = te * 3;
            idx++;
        }
    }

    return idx;
}
