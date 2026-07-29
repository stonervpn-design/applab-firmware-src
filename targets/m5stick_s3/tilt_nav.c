#include "tilt_nav.h"
#include <math.h>

float tilt_roll_deg(int16_t a_roll, int16_t a_up) {
    /* fabs on the up axis so upright (a_roll==0) is 0 deg for either mounting sign. */
    return atan2f((float)a_roll, fabsf((float)a_up)) * 57.2957795f;
}

TiltDir tilt_eval(int16_t a_roll, int16_t a_up, uint32_t now_ms, TiltState* st) {
    float roll = tilt_roll_deg(a_roll, a_up);
    float mag = roll < 0.0f ? -roll : roll;
    TiltDir cur = (roll >= 0.0f) ? TiltRight : TiltLeft;
#if TILT_INVERT
    cur = (cur == TiltRight) ? TiltLeft : TiltRight;
#endif

    if(!st->engaged) {
        if(mag >= TILT_FIRE_DEG) {
            st->engaged = true;
            st->dir = cur;
            st->last_repeat_at = now_ms;
            return cur; /* first emit */
        }
        return TiltNone;
    }

    /* engaged: released or flipped -> disengage (a later tick may re-fire) */
    if(mag < TILT_RELEASE_DEG || cur != st->dir) {
        st->engaged = false;
        return TiltNone;
    }

    /* held past release in same dir -> repeat on cadence */
    if((uint32_t)(now_ms - st->last_repeat_at) >= TILT_REPEAT_MS) {
        st->last_repeat_at = now_ms;
        return st->dir;
    }
    return TiltNone;
}
