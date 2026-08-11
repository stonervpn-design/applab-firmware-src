/**
 * target_input.c — M5Stack StickS3 two-button input driver
 *
 * Two physical buttons (KEY1=GPIO11 "M5"/front, KEY2=GPIO12 side). The control layout
 * matches Bruce (github pr3y/Bruce, boards/m5stack-sticks3) so it's familiar:
 *
 *   KEY1 (M5/front): press            -> InputKeyOk    (Select)
 *   KEY2 (side/DW):  single-click     -> InputKeyDown
 *                    double-click     -> InputKeyUp     (2nd click within 250ms)
 *                    long-press >=600ms -> InputKeyBack (SHORT — pops a scene)
 *                    keep holding >=1200ms -> InputKeyBack (LONG — game exit, e.g.
 *                                            Snake; an extension beyond Bruce)
 *   Screen wake:     any deliberate BMI270 movement wakes the screen once it has
 *                    gone dark from inactivity — and does nothing else while dark.
 *   Tilt nav:        once the screen is already lit, a firm roll of the stick past a
 *                    threshold emits one InputKeyLeft/Right tap. Hysteresis (the roll
 *                    must fall back into a neutral band before it can fire again) plus a
 *                    cooldown keep ordinary handling from injecting nav. A roll made
 *                    while the screen is dark only wakes it — it never also navigates.
 *                    Honors the s_tilt_enabled toggle (System settings).
 *
 * A single-click is confirmed only after the 250ms double-click window closes, so Down
 * has a slight, deliberate lag (Bruce does the same) to distinguish it from Up.
 *
 * Buttons are wired to GND and read with the internal pull-up: pressed == level 0.
 */

#include "target_input.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <input.h>
#include <driver/gpio.h>
#include <nvs.h>
#include "boards/board.h"
#include "bmi270.h"

#define TAG "InputStickS3"

#define BTN_KEY1_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY1)
#define BTN_KEY2_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY2)

/* Bruce control scheme (github pr3y/Bruce, boards/m5stack-sticks3):
 *   KEY1 (GPIO11, "M5"/front button) = Select/Ok.
 *   KEY2 (GPIO12, side button)       = nav: single-click Down, double-click Up,
 *                                      long-press (>=600ms) Back.
 * The BMI270 IMU wakes the screen from inactivity (our addition; Bruce has no tilt) and,
 * once the screen is lit, injects Left/Right on a firm roll — the only nav keys the
 * two physical buttons cannot otherwise produce. While the screen is dark the IMU only
 * wakes; it never injects navigation keys in the dark.
 * DW_LONG2 is an extension beyond Bruce: keep holding past it for a LONG Back, which
 * the bundled Flipper games (Snake, ...) require to exit — Bruce's Arduino apps don't
 * use that convention so upstream stops at one Back. */
#define DW_DOUBLE_MS     250U  /* max gap first-release -> second-release for a double */
#define DW_LONG_MS       600U  /* hold -> Back (Bruce Esc) */
#define DW_LONG2_MS      1200U /* keep holding -> long Back (game exit) */
#define KEY_DEBOUNCE_MS  20U
#define ACCEL_SAMPLE_MS  40U   /* throttle BMI270 reads: 25 Hz is ample for wake-detect
                                * vs. reading every 4 ms poll (~250 Hz) on the shared I2C bus */

static void publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t* seq) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_KEYBOARD,
        .sequence_counter = ++(*seq),
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

/* Emit a complete synthetic short press (Press -> Short -> Release). */
static void publish_tap(FuriPubSub* pubsub, InputKey key, uint32_t* seq) {
    publish(pubsub, key, InputTypePress, seq);
    publish(pubsub, key, InputTypeShort, seq);
    publish(pubsub, key, InputTypeRelease, seq);
}

/* Emit a complete synthetic long press (Press -> Long -> Release). */
static void publish_long(FuriPubSub* pubsub, InputKey key, uint32_t* seq) {
    publish(pubsub, key, InputTypePress, seq);
    publish(pubsub, key, InputTypeLong, seq);
    publish(pubsub, key, InputTypeRelease, seq);
}

typedef struct {
    bool pressed; /* debounced logical state */
    bool raw_last; /* last raw sample */
    uint32_t changed_at; /* tick of last raw change (debounce) */
    uint32_t press_started_at;
    /* KEY2 (side/DW nav) single-vs-double-vs-long tracking */
    bool waiting; /* first click released, awaiting a possible second click */
    uint32_t first_release_at;
    bool long_fired; /* short Back already emitted this hold */
    bool long2_fired; /* long Back already emitted this hold */
} BtnState;

static BtnState s_key1;
static BtnState s_key2;

static bool s_ready = false;

/* ---- BMI270 motion -> wake the screen (only; never navigation) ---- */
static volatile bool s_tilt_enabled = true; /* default on; System settings persists it */

/* Deliberate-motion detector. We track a slow baseline of the resting accel vector;
 * a large deviation from it means the stick was picked up / tilted / shaken. The
 * baseline follows the resting pose on a time gate (not per-sample), so detection
 * is independent of the input poll rate. At ±2g, 1g ≈ 16384 LSB. */
#define WAKE_DEV_THRESH    3000   /* L1 deviation to count as movement (~0.18 g) */
#define WAKE_REFRACTORY_MS 1500U  /* min gap between wakes, and settle time for the baseline */
#define WAKE_BASE_STEP_MS  100U   /* baseline chases the resting pose at ~10 Hz */
#define WAKE_KEY           InputKeyOk /* wake carries a key but only as Press+Release (no Short/Long), so no view acts on it */

static int32_t s_acc_base[3];
static bool s_acc_base_have = false;
static uint32_t s_acc_base_at = 0;
static uint32_t s_wake_last_at = 0;

void target_input_set_tilt_enabled(bool enabled) {
    s_tilt_enabled = enabled;
    if(!enabled) s_acc_base_have = false; /* re-seed the baseline next time it's on */
    /* persist so it survives reboot (read back in target_input_init) */
    nvs_handle_t h;
    if(nvs_open("input", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "tilt_nav", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool target_input_get_tilt_enabled(void) {
    return s_tilt_enabled;
}

/* Returns true once per deliberate movement. A table-resting stick keeps deviation
 * near zero (no wake); a pick-up/tilt spikes it past the threshold. The refractory
 * window plus the baseline catching up mean each movement wakes at most once. */
static bool motion_wake_edge(int16_t x, int16_t y, int16_t z, uint32_t now) {
    if(!s_acc_base_have) {
        s_acc_base[0] = x;
        s_acc_base[1] = y;
        s_acc_base[2] = z;
        s_acc_base_have = true;
        s_acc_base_at = now;
        return false;
    }
    int32_t dx = (int32_t)x - s_acc_base[0];
    int32_t dy = (int32_t)y - s_acc_base[1];
    int32_t dz = (int32_t)z - s_acc_base[2];
    int32_t dev = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);
    if((uint32_t)(now - s_acc_base_at) >= WAKE_BASE_STEP_MS) {
        s_acc_base[0] += dx / 4;
        s_acc_base[1] += dy / 4;
        s_acc_base[2] += dz / 4;
        s_acc_base_at = now;
    }
    if(dev > WAKE_DEV_THRESH && (uint32_t)(now - s_wake_last_at) >= WAKE_REFRACTORY_MS) {
        s_wake_last_at = now;
        return true;
    }
    return false;
}

/* ---- BMI270 firm-roll -> Left/Right navigation (only while the screen is lit) ----
 * Authorized addition (the wake path above stays wake-only). One deliberate roll about
 * the stick's long axis, held past a firm threshold, emits a single InputKeyLeft/Right
 * tap. Two guardrails stop ordinary handling / wobble from injecting nav:
 *   - hysteresis: after firing we DISARM and only re-arm once the roll falls back into a
 *     neutral band (NEUTRAL << FIRE), so a sustained tilt fires exactly once;
 *   - cooldown: a minimum gap between taps.
 * `roll` is the gravity component along the roll axis in raw ±2g LSB (1g ≈ 16384).
 * NOTE (needs on-device tuning): the thresholds below and the axis/sign chosen in
 * target_input_poll() (currently ax, +roll = Right) are best-guess defaults — the axis
 * may need swapping to ay and/or the sign negating depending on the BMI270 mounting. */
#define TILT_NAV_FIRE_LSB    7000   /* |roll| past this = a firm tilt (~0.43 g, ~25°) */
#define TILT_NAV_NEUTRAL_LSB 3000   /* must fall back below this to re-arm (hysteresis) */
#define TILT_NAV_COOLDOWN_MS 400U   /* min gap between tilt-nav taps */

static bool s_tilt_nav_armed = true;      /* true when roll is in the neutral band, ready to fire */
static uint32_t s_tilt_nav_last_at = 0;   /* tick of the last emitted tilt-nav tap */

/* Force the detector back to its armed/neutral rest state. Called while the screen is
 * dark so a roll performed in the dark cannot fire on the freshly-lit view. */
static void tilt_nav_reset(void) {
    /* DISARM: a roll made while the screen is dark must not fire on the freshly-lit
     * view. armed re-asserts only after the roll falls back into the neutral band. */
    s_tilt_nav_armed = false;
}

/* Emit at most one Left/Right tap per firm roll. Direction comes from the sign of roll. */
static void tilt_nav_edge(FuriPubSub* pubsub, int16_t roll, uint32_t now, uint32_t* seq) {
    int32_t mag = roll < 0 ? -(int32_t)roll : (int32_t)roll;
    if(s_tilt_nav_armed && mag >= TILT_NAV_FIRE_LSB &&
       (uint32_t)(now - s_tilt_nav_last_at) >= furi_ms_to_ticks(TILT_NAV_COOLDOWN_MS)) {
        /* firm roll past threshold, armed, and past cooldown -> one tap */
        publish_tap(pubsub, roll > 0 ? InputKeyRight : InputKeyLeft, seq);
        s_tilt_nav_last_at = now;
        s_tilt_nav_armed = false; /* disarm until the roll returns to the neutral band */
    } else if(!s_tilt_nav_armed && mag <= TILT_NAV_NEUTRAL_LSB) {
        s_tilt_nav_armed = true;  /* back in neutral -> ready to fire again */
    }
}

void target_input_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BTN_KEY1) | (1ULL << BOARD_PIN_BTN_KEY2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_key1 = (BtnState){0};
    s_key2 = (BtnState){0};
    s_key1.raw_last = false;
    s_key2.raw_last = false;
    s_ready = true;
    FURI_LOG_I(TAG, "StickS3 2-button input init (KEY1=GPIO%d KEY2=GPIO%d)",
               (int)BOARD_PIN_BTN_KEY1, (int)BOARD_PIN_BTN_KEY2);

    s_acc_base_have = false;
    s_tilt_nav_armed = true;
    s_tilt_nav_last_at = 0;
    if(bmi270_init()) {
        FURI_LOG_I(TAG, "BMI270 available; tilt-to-wake enabled");
    } else {
        FURI_LOG_I(TAG, "BMI270 not available; tilt-to-wake disabled");
    }

    /* apply persisted tilt-nav toggle (default ON if unset) */
    {
        nvs_handle_t h;
        uint8_t v = 1;
        if(nvs_open("input", NVS_READONLY, &h) == ESP_OK) {
            if(nvs_get_u8(h, "tilt_nav", &v) != ESP_OK) v = 1;
            nvs_close(h);
        }
        s_tilt_enabled = (v != 0);
    }
}

/* Debounce a raw sample into btn->pressed; returns true if the debounced state
 * just transitioned, with *now_pressed set to the new state. */
static bool debounce(BtnState* btn, bool raw_now, uint32_t now, bool* now_pressed) {
    if(raw_now != btn->raw_last) {
        btn->raw_last = raw_now;
        btn->changed_at = now;
        return false;
    }
    if(raw_now != btn->pressed &&
       (now - btn->changed_at) >= furi_ms_to_ticks(KEY_DEBOUNCE_MS)) {
        btn->pressed = raw_now;
        *now_pressed = raw_now;
        return true;
    }
    return false;
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    if(!s_ready) return;
    const uint32_t now = furi_get_tick();
    const uint32_t double_ticks = furi_ms_to_ticks(DW_DOUBLE_MS);
    const uint32_t long_ticks = furi_ms_to_ticks(DW_LONG_MS);
    const uint32_t long2_ticks = furi_ms_to_ticks(DW_LONG2_MS);

    /* Active-low: pressed == level 0 */
    bool raw1 = (gpio_get_level(BTN_KEY1_GPIO) == 0);
    bool raw2 = (gpio_get_level(BTN_KEY2_GPIO) == 0);

    /* ---- KEY1 (M5/front) = Select/Ok on each press ---- */
    bool changed1, state1 = false;
    changed1 = debounce(&s_key1, raw1, now, &state1);
    if(changed1 && state1) {
        publish_tap(pubsub, InputKeyOk, sequence_counter);
    }

    /* ---- KEY2 (side/DW) = single-click Down / double-click Up / long-press Back ---- */
    bool changed2, state2 = false;
    changed2 = debounce(&s_key2, raw2, now, &state2);
    if(changed2) {
        if(state2) {
            /* press edge */
            s_key2.press_started_at = now;
            s_key2.long_fired = false;
            s_key2.long2_fired = false;
        } else if(!s_key2.long_fired) {
            /* short click released (never became a long-press) */
            if(s_key2.waiting && (now - s_key2.first_release_at) <= double_ticks) {
                /* second click within the window -> double-click -> Up */
                publish_tap(pubsub, InputKeyUp, sequence_counter);
                s_key2.waiting = false;
            } else {
                /* first click -> open the double-click window */
                s_key2.waiting = true;
                s_key2.first_release_at = now;
            }
        } else {
            /* release after a long-press -> nothing more */
            s_key2.waiting = false;
        }
    } else if(s_key2.pressed) {
        uint32_t held = now - s_key2.press_started_at;
        if(!s_key2.long_fired && held >= long_ticks) {
            /* held past the long threshold -> Back (short, pops a scene) */
            s_key2.long_fired = true;
            s_key2.waiting = false; /* a long-press cancels a pending single-click */
            publish_tap(pubsub, InputKeyBack, sequence_counter);
        } else if(s_key2.long_fired && !s_key2.long2_fired && held >= long2_ticks) {
            /* keep holding -> long Back, for games that gate exit on InputTypeLong */
            s_key2.long2_fired = true;
            publish_long(pubsub, InputKeyBack, sequence_counter);
        }
    }

    /* single-click confirmation: released, no second click arrived within the window */
    if(!s_key2.pressed && s_key2.waiting &&
       (now - s_key2.first_release_at) > double_ticks) {
        publish_tap(pubsub, InputKeyDown, sequence_counter);
        s_key2.waiting = false;
    }

    /* ---- BMI270 motion -> wake the screen (never navigation) ----
     * Throttle to ACCEL_SAMPLE_MS: the accel is on the shared keyboard/sensor I2C bus,
     * so reading it every 4 ms poll floods the bus for a wake-detector that needs ~25 Hz. */
    static uint32_t s_last_accel_tick = 0;
    if(s_tilt_enabled && bmi270_is_present() &&
       (uint32_t)(now - s_last_accel_tick) >= furi_ms_to_ticks(ACCEL_SAMPLE_MS)) {
        s_last_accel_tick = now;
        int16_t ax = 0, ay = 0, az = 0;
        if(bmi270_read_accel(&ax, &ay, &az)) {
            /* CONFIG_FREERTOS_HZ=1000 -> 1 tick == 1 ms, so `now` is already ms. */
            bool dark = (furi_hal_light_get_backlight() == 0);
            /* Wake path (behavior unchanged): motion_wake_edge() runs on every sample so
             * the baseline keeps chasing the resting pose (the && only short-circuits
             * AFTER it executes); a wake is emitted only while the screen is dark. */
            if(motion_wake_edge(ax, ay, az, now) && dark) {
                /* Screen is dark: emit one wake. A bare Press+Release (no Short/Long)
                 * turns the backlight back on via the notification service and resets
                 * the desktop idle timer, but no view treats it as a keypress — so tilt
                 * wakes the screen and does nothing else while dark. */
                publish(pubsub, WAKE_KEY, InputTypePress, sequence_counter);
                publish(pubsub, WAKE_KEY, InputTypeRelease, sequence_counter);
            }
            /* Tilt navigation is live only while the screen is already lit. While dark we
             * hold the detector disarmed (tilt_nav_reset) so a roll performed in the dark
             * wakes the screen but never also injects a Left/Right on the freshly-lit view. */
            if(!dark) {
                tilt_nav_edge(pubsub, ax, now, sequence_counter);
            } else {
                tilt_nav_reset();
            }
        }
    }
}
