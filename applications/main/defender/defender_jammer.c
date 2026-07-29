#include "defender_jammer.h"
#include <math.h>

void defender_jammer_feed(
    DefenderJammerState* st,
    float rssi,
    bool packet_decoded,
    uint32_t now_ms) {

    // Initialize floor on first sample
    if(!st->have_floor) {
        st->floor_ema = rssi;
        st->have_floor = true;
    }

    // Update noise floor using asymmetric EMA:
    // - Fast chase downward when signal drops (50/50 blend)
    // - Slow drift upward when signal rises (99/1 blend)
    if(rssi < st->floor_ema) {
        st->floor_ema = st->floor_ema * 0.5f + rssi * 0.5f;
    } else {
        st->floor_ema = st->floor_ema * 0.99f + rssi * 0.01f;
    }

    // Jam detection threshold
    float thresh = st->floor_ema + JAM_DB_OVER;

    // State machine:
    // - If signal is over threshold AND no packet decoded, measure sustain time
    // - Reset timer if signal drops below threshold OR packet decoded
    if(rssi >= thresh && !packet_decoded) {
        // Over-threshold with no packet → accumulate time
        if(st->over_since_ms == 0) {
            // First time over-threshold in this event
            st->over_since_ms = now_ms;
            st->jamming = false;
        } else {
            // Check if sustained long enough
            uint32_t duration = now_ms - st->over_since_ms;
            if(duration >= JAM_SUSTAIN_MS) {
                st->jamming = true;
            }
        }
    } else {
        // Signal dropped below threshold OR packet decoded → reset
        st->over_since_ms = 0;
        st->jamming = false;
    }
}
