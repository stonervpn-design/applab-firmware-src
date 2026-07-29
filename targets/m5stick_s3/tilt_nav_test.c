/* Host test for tilt_nav.c — build: cc -o tilt_test tilt_nav_test.c tilt_nav.c -lm */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "tilt_nav.h"

/* Expected Left/Right for a positive (right) / negative (left) physical roll, honoring
 * the board's TILT_INVERT calibration so the test passes for either value. */
#if TILT_INVERT
#define EXP_RIGHT TiltLeft
#define EXP_LEFT  TiltRight
#else
#define EXP_RIGHT TiltRight
#define EXP_LEFT  TiltLeft
#endif

/* ~+/-2g range: 1g ~= 16384 LSB. Upright => a_up ~ +1g, a_roll ~ 0. */
#define G 16384
/* accel components for a given roll angle (deg): a_roll = sin, a_up = cos */
static int16_t rroll(double deg) { return (int16_t)(G * sin(deg * 3.14159265 / 180.0)); }
static int16_t rup(double deg)   { return (int16_t)(G * cos(deg * 3.14159265 / 180.0)); }

int main(void) {
    TiltState st = {0};

    /* upright -> no key, not engaged */
    assert(tilt_eval(0, G, 0, &st) == TiltNone);
    assert(!st.engaged);

    /* 30 deg right is below FIRE(35) -> still nothing */
    assert(tilt_eval(rroll(30), rup(30), 10, &st) == TiltNone);
    assert(!st.engaged);

    /* 45 deg right crosses FIRE -> emit Right once, now engaged */
    assert(tilt_eval(rroll(45), rup(45), 20, &st) == EXP_RIGHT);
    assert(st.engaged && st.dir == EXP_RIGHT);

    /* immediately again (only 5ms later) -> no repeat yet (REPEAT=150) */
    assert(tilt_eval(rroll(45), rup(45), 25, &st) == TiltNone);

    /* 160ms after first emit -> repeat Right */
    assert(tilt_eval(rroll(45), rup(45), 180, &st) == EXP_RIGHT);

    /* drop to 30 deg: still >= RELEASE(25) -> stays engaged, no key between repeats */
    assert(tilt_eval(rroll(30), rup(30), 200, &st) == TiltNone);
    assert(st.engaged);

    /* drop to 20 deg: below RELEASE -> disengage */
    assert(tilt_eval(rroll(20), rup(20), 210, &st) == TiltNone);
    assert(!st.engaged);

    /* roll left 45 deg -> emit Left */
    assert(tilt_eval(rroll(-45), rup(-45), 220, &st) == EXP_LEFT);
    assert(st.engaged && st.dir == EXP_LEFT);

    /* flip straight to right 45 without recentering -> that tick disengages... */
    assert(tilt_eval(rroll(45), rup(45), 230, &st) == TiltNone);
    assert(!st.engaged);
    /* ...next tick re-fires as Right */
    assert(tilt_eval(rroll(45), rup(45), 240, &st) == EXP_RIGHT);
    assert(st.dir == EXP_RIGHT);

    /* up-axis sign independence: upright with a_up NEGATIVE is still ~0 deg */
    TiltState st2 = {0};
    assert(tilt_eval(0, -G, 0, &st2) == TiltNone);

    printf("tilt_nav_test: ALL PASS\n");
    return 0;
}
