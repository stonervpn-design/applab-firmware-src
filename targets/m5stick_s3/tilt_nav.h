#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Tilt-to-direction mapping for the 2-button M5Stick S3. Pure logic, host-testable. */

typedef enum { TiltNone = 0, TiltLeft = 1, TiltRight = 2 } TiltDir;

typedef struct {
    bool engaged;            /* currently past the FIRE threshold in `dir` */
    TiltDir dir;             /* the direction we engaged in */
    uint32_t last_repeat_at; /* ms tick of the last emitted key */
} TiltState;

/* Tuning (on-device tunable). Angles in degrees from upright. */
#define TILT_FIRE_DEG      35.0f
#define TILT_RELEASE_DEG   25.0f   /* hysteresis: must fall below this to disengage */
#define TILT_REPEAT_MS     150u

/* Swap which roll direction is Left vs Right. On-device (2026-07-28) a physical
 * right-roll produced Left, so this board's mounting needs the inversion. */
#define TILT_INVERT        1

/* Signed roll angle in degrees. a_up reads ~+/-1g upright; a_roll swings with L/R roll.
 * Uses |a_up| so an upright reading is ~0 deg regardless of the up-axis sign. */
float tilt_roll_deg(int16_t a_roll, int16_t a_up);

/* Evaluate one accel sample. Returns the key to emit THIS tick, or TiltNone.
 * All state lives in *st; `now_ms` is a millisecond clock. */
TiltDir tilt_eval(int16_t a_roll, int16_t a_up, uint32_t now_ms, TiltState* st);
