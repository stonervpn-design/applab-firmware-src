/**
 * target_input.c — M5Stack StickS3 two-button input driver
 *
 * Only two physical buttons (KEY1=GPIO11 front, KEY2=GPIO12 side). A usable 5-key
 * Flipper nav is mapped onto them via tap / double-tap / hold:
 *
 *   KEY2 (side)  = NAVIGATION:  tap = Down,  double-tap = Up,   hold = fast Down repeat
 *   KEY1 (front) = OK / ACTION: tap = Ok,    double-tap = Right, hold = Back
 *
 * (Swapped from the old layout — nav is now the side button, Ok the front button —
 * and double-tap adds Up + Right so menus scroll both ways and setting lists can
 * change values. Right wraps through a list's values, so Left isn't needed.)
 *
 * A short tap is resolved after a brief double-tap window (so a second tap can turn
 * it into the double-tap key); hold-repeat gives fast scrolling so the small
 * single-tap delay only ever affects single steps. Buttons are active-low (internal
 * pull-up; pressed == level 0). Up/Left arrive later via BMI270 tilt (Phase 1).
 */

#include "target_input.h"

#include <furi.h>
#include <input.h>
#include <driver/gpio.h>
#include "boards/board.h"

#define TAG "InputStickS3"

#define BTN_KEY1_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY1) /* front */
#define BTN_KEY2_GPIO ((gpio_num_t)BOARD_PIN_BTN_KEY2) /* side  */

#define INPUT_LONG_PRESS_MS 500U /* hold threshold (Back / repeat) */
#define INPUT_REPEAT_MS     150U /* fast-scroll step */
#define INPUT_DOUBLE_MS     250U /* max gap for a double-tap */
#define KEY_DEBOUNCE_MS     30U

static void publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t* seq) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_KEYBOARD,
        .sequence_counter = ++(*seq),
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

/* Emit a complete synthetic tap (Press -> Short -> Release) for a resolved key. */
static void publish_tap(FuriPubSub* pubsub, InputKey key, uint32_t* seq) {
    publish(pubsub, key, InputTypePress, seq);
    publish(pubsub, key, InputTypeShort, seq);
    publish(pubsub, key, InputTypeRelease, seq);
}

/* Per-button config: what each gesture resolves to. */
typedef struct {
    InputKey single; /* short tap */
    InputKey dbl; /* double tap */
    InputKey hold_key; /* one-shot hold action (ignored when hold_repeats) */
    bool hold_repeats; /* true: repeat `single` while held; false: emit hold_key once */
} BtnCfg;

typedef struct {
    bool pressed; /* debounced logical state */
    bool raw_last; /* last raw sample */
    uint32_t changed_at; /* tick of last raw change (debounce) */
    uint32_t press_at;
    uint32_t last_rep_at;
    uint32_t released_at;
    bool long_done; /* hold action started (Back emitted / repeat running) */
    bool await_dbl; /* released, waiting to see if a 2nd tap makes a double */
    bool skip_release; /* 2nd tap of a double: swallow its release */
} BtnState;

static const BtnCfg CFG_NAV = {
    .single = InputKeyDown,
    .dbl = InputKeyUp,
    .hold_key = InputKeyDown,
    .hold_repeats = true,
};
static const BtnCfg CFG_OK = {
    .single = InputKeyOk,
    .dbl = InputKeyRight,
    .hold_key = InputKeyBack,
    .hold_repeats = false,
};

static BtnState s_nav; /* KEY2 side */
static BtnState s_ok; /* KEY1 front */
static bool s_ready = false;

void target_input_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BTN_KEY1) | (1ULL << BOARD_PIN_BTN_KEY2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_nav = (BtnState){0};
    s_ok = (BtnState){0};
    s_ready = true;
    FURI_LOG_I(
        TAG,
        "StickS3 input: side(GPIO%d)=Nav, front(GPIO%d)=Ok (dbl-tap=Up/Right)",
        (int)BOARD_PIN_BTN_KEY2,
        (int)BOARD_PIN_BTN_KEY1);
}

/* Debounce a raw sample; returns true when the debounced state transitions,
 * with *now_pressed set to the new state. */
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

static void process_button(
    FuriPubSub* pubsub,
    uint32_t* seq,
    BtnState* s,
    const BtnCfg* c,
    bool raw,
    uint32_t now,
    uint32_t long_t,
    uint32_t rep_t,
    uint32_t dbl_t) {
    bool state = false;
    if(debounce(s, raw, now, &state)) {
        if(state) {
            /* pressed */
            if(s->await_dbl) {
                /* second tap within the window -> double-tap */
                publish_tap(pubsub, c->dbl, seq);
                s->await_dbl = false;
                s->skip_release = true; /* don't let this press start a new single */
                s->long_done = true; /* nor a hold */
            } else {
                s->press_at = now;
                s->long_done = false;
                s->skip_release = false;
            }
        } else {
            /* released */
            if(s->skip_release) {
                s->skip_release = false;
            } else if(!s->long_done) {
                s->await_dbl = true; /* defer the single until the double window passes */
                s->released_at = now;
            }
        }
    } else if(s->pressed && !s->long_done && (now - s->press_at) >= long_t) {
        /* held past threshold */
        s->long_done = true;
        if(c->hold_repeats) {
            s->last_rep_at = now;
            publish_tap(pubsub, c->single, seq); /* first fast-scroll step */
        } else {
            publish_tap(pubsub, c->hold_key, seq); /* Back */
        }
    } else if(
        s->pressed && s->long_done && c->hold_repeats && !s->skip_release &&
        (now - s->last_rep_at) >= rep_t) {
        /* !skip_release: after a double-tap the 2nd tap is held+swallowed; without
         * this guard its stale last_rep_at makes the repeat fire once immediately,
         * emitting `single` right after `dbl` (double-tap Up then a stray Down). */
        s->last_rep_at = now;
        publish_tap(pubsub, c->single, seq); /* fast-scroll repeat */
    }

    /* resolve a pending single tap once the double-tap window has elapsed */
    if(s->await_dbl && (now - s->released_at) >= dbl_t) {
        s->await_dbl = false;
        publish_tap(pubsub, c->single, seq);
    }
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    if(!s_ready) return;
    const uint32_t now = furi_get_tick();
    const uint32_t long_t = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    const uint32_t rep_t = furi_ms_to_ticks(INPUT_REPEAT_MS);
    const uint32_t dbl_t = furi_ms_to_ticks(INPUT_DOUBLE_MS);

    /* Active-low: pressed == level 0 */
    bool raw_nav = (gpio_get_level(BTN_KEY2_GPIO) == 0); /* side  = Nav */
    bool raw_ok = (gpio_get_level(BTN_KEY1_GPIO) == 0); /* front = Ok  */

    process_button(pubsub, sequence_counter, &s_nav, &CFG_NAV, raw_nav, now, long_t, rep_t, dbl_t);
    process_button(pubsub, sequence_counter, &s_ok, &CFG_OK, raw_ok, now, long_t, rep_t, dbl_t);
}
