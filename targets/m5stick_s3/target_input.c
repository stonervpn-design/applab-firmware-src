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
 *   Left/Right:      BMI270 tilt (roll the stick) — see tilt_nav.c.
 *
 * A single-click is confirmed only after the 250ms double-click window closes, so Down
 * has a slight, deliberate lag (Bruce does the same) to distinguish it from Up.
 *
 * Buttons are wired to GND and read with the internal pull-up: pressed == level 0.
 */

#include "target_input.h"

#include <furi.h>
#include <input.h>
#include <driver/gpio.h>
#include <nvs.h>
#include "boards/board.h"
#include "bmi270.h"
#include "tilt_nav.h"

#define TAG "InputStickS3"

#define BTN_KEY1_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY1)
#define BTN_KEY2_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY2)

/* Bruce control scheme (github pr3y/Bruce, boards/m5stack-sticks3):
 *   KEY1 (GPIO11, "M5"/front button) = Select/Ok.
 *   KEY2 (GPIO12, side button)       = nav: single-click Down, double-click Up,
 *                                      long-press (>=600ms) Back.
 * Left/Right come from the BMI270 tilt (our addition; Bruce has no tilt).
 * DW_LONG2 is an extension beyond Bruce: keep holding past it for a LONG Back, which
 * the bundled Flipper games (Snake, ...) require to exit — Bruce's Arduino apps don't
 * use that convention so upstream stops at one Back. */
#define DW_DOUBLE_MS     250U  /* max gap first-release -> second-release for a double */
#define DW_LONG_MS       600U  /* hold -> Back (Bruce Esc) */
#define DW_LONG2_MS      1200U /* keep holding -> long Back (game exit) */
#define KEY_DEBOUNCE_MS  20U

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

/* ---- BMI270 tilt -> Left/Right ---- */
static volatile bool s_tilt_enabled = true; /* default on; System settings persists it */
static TiltState s_tilt;

void target_input_set_tilt_enabled(bool enabled) {
    s_tilt_enabled = enabled;
    if(!enabled) s_tilt = (TiltState){0}; /* drop any held repeat */
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

/* Which raw accel axis rolls with Left/Right, and which reads ~1g upright. Confirm
 * on-device by tilting; if reversed, swap these / set TILT_INVERT in tilt_nav.h. */
#define TILT_ROLL_AXIS(x, y, z) (x) /* a_roll */
#define TILT_UP_AXIS(x, y, z)   (z) /* a_up */

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

    s_tilt = (TiltState){0};
    if(bmi270_init()) {
        FURI_LOG_I(TAG, "BMI270 tilt-nav available");
    } else {
        FURI_LOG_I(TAG, "BMI270 not available; tilt-nav disabled");
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

    /* ---- BMI270 tilt -> Left/Right ---- */
    if(s_tilt_enabled && bmi270_is_present()) {
        int16_t ax = 0, ay = 0, az = 0;
        if(bmi270_read_accel(&ax, &ay, &az)) {
            (void)ay; /* only the roll+up axes are used; keep all three for easy remap */
            /* CONFIG_FREERTOS_HZ=1000 -> 1 tick == 1 ms, so `now` is already ms. */
            TiltDir d = tilt_eval(
                TILT_ROLL_AXIS(ax, ay, az), TILT_UP_AXIS(ax, ay, az), now, &s_tilt);
            if(d == TiltLeft) {
                publish_tap(pubsub, InputKeyLeft, sequence_counter);
            } else if(d == TiltRight) {
                publish_tap(pubsub, InputKeyRight, sequence_counter);
            }
        }
    }
}
