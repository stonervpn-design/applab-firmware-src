/**
 * target_input.c — LilyGO T-Embed CC1101 rotary-encoder input driver
 *
 * Hardware: an EC11-style rotary encoder plus two momentary buttons, all wired to
 * GND and read with internal pull-ups (pressed / active == level 0).
 *
 *   Encoder A/B (GPIO4 / GPIO5): quadrature rotation.
 *   Encoder push (GPIO0)       : Ok  (full Press/Long/Short/Repeat/Release stream).
 *   Back button  (GPIO6)       : Back  (and the "shift" modifier, see below).
 *
 * Nav mapping — SINGLE AXIS + per-view remap (NOT the old dual-axis hack):
 *   Plain turn            -> Up (CCW) / Down (CW), tagged INPUT_SEQUENCE_SOURCE_HARDWARE.
 *   Back held + turn      -> Left (CCW) / Right (CW), tagged INPUT_SEQUENCE_SOURCE_KEYBOARD.
 *
 * Why: the view dispatcher already remaps a single HARDWARE axis per the focused
 * view's ViewInputMode (view_dispatcher.c). So a plain turn is context-aware for
 * free — it scrolls Up/Down in vertical lists/menus, and the same turn is remapped
 * to Left/Right inside horizontal button bars (widgets with buttons, dialogs,
 * Read RAW, ...). That means plain turning NEVER injects a raw Left/Right into a
 * vertical menu, which is what used to make "scroll left" jump into menus.
 *
 * The one thing a single axis cannot do on its own is drive a genuine 2-D view
 * (the number pad, date/time setup, SubBrute repeat count, TPMS actions, KeeLoq
 * fob grid, byte-input mini-editor). For those, hold Back and turn: that emits raw
 * Left/Right tagged KEYBOARD, which bypasses the per-view remap and reaches the
 * second axis directly. A turn while Back is held suppresses the Back tap, so the
 * modifier never doubles as a back-navigation.
 *
 * Ok (encoder push) is deliberately left as a pure button so long-press Ok
 * (e.g. KeeLoq "Sync") keeps working.
 *
 * Called every INPUT_POLL_MS (4 ms) from the input service — fast enough to catch
 * every quadrature transition of a hand-turned encoder, so no GPIO ISR is needed.
 *
 * Flip ENC_DIR if scrolling goes the wrong way; adjust ENC_COUNTS_PER_DETENT
 * (4 or 2 for most EC11s) if one physical click scrolls by the wrong number of steps.
 */

#include "target_input.h"

#include <furi.h>
#include <input.h>
#include <driver/gpio.h>
#include "boards/board.h"

#define TAG "InputTEmbed"

#define ENC_A_GPIO   ((gpio_num_t)BOARD_PIN_ENCODER_A)
#define ENC_B_GPIO   ((gpio_num_t)BOARD_PIN_ENCODER_B)
#define BTN_OK_GPIO  ((gpio_num_t)BOARD_PIN_ENCODER_BTN)
#define BTN_BK_GPIO  ((gpio_num_t)BOARD_PIN_BUTTON_KEY)

/* +1 = CW scrolls Down/Right; flip to -1 if the encoder scrolls the wrong way. */
#define ENC_DIR                 (+1)
/* Quadrature counts per physical detent (EC11: usually 4, some 2). */
#define ENC_COUNTS_PER_DETENT   4

#define INPUT_LONG_PRESS_MS 500U
#define INPUT_REPEAT_MS     150U
#define KEY_DEBOUNCE_MS     30U

static void
    publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t* seq, uint8_t source) {
    InputEvent event = {
        .sequence_source = source,
        .sequence_counter = ++(*seq),
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

/* Emit a complete synthetic tap (Press -> Short -> Release) for a resolved key. */
static void publish_tap(FuriPubSub* pubsub, InputKey key, uint32_t* seq, uint8_t source) {
    publish(pubsub, key, InputTypePress, seq, source);
    publish(pubsub, key, InputTypeShort, seq, source);
    publish(pubsub, key, InputTypeRelease, seq, source);
}

typedef struct {
    bool pressed; /* debounced logical state */
    bool raw_last; /* last raw sample */
    uint32_t changed_at; /* tick of last raw change (debounce) */
    uint32_t press_started_at;
    bool long_fired; /* long threshold already handled this hold */
} BtnState;

static BtnState s_ok;
static BtnState s_back;

/* A turn happened while Back was held -> this hold is a Left/Right shift, so its
 * eventual Back tap must be suppressed. */
static bool s_back_shift_used;

/* Quadrature decoder state */
static uint8_t s_enc_prev; /* last (A<<1|B) 2-bit reading */
static int8_t s_enc_accum; /* signed sub-step accumulator */

static bool s_ready = false;

/* Transition table: index = (prev_state<<2)|new_state, prev/new = (A<<1|B).
 * Yields +1 / -1 for the 8 valid gray-code transitions, 0 for none/invalid. */
static const int8_t s_qdec[16] = {
    0, -1, +1, 0,
    +1, 0, 0, -1,
    -1, 0, 0, +1,
    0, +1, -1, 0};

void target_input_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_ENCODER_A) | (1ULL << BOARD_PIN_ENCODER_B) |
                        (1ULL << BOARD_PIN_ENCODER_BTN) | (1ULL << BOARD_PIN_BUTTON_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_ok = (BtnState){0};
    s_back = (BtnState){0};
    s_back_shift_used = false;
    s_enc_accum = 0;
    s_enc_prev = (uint8_t)((gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO));
    s_ready = true;
    FURI_LOG_I(
        TAG,
        "T-Embed encoder input init (A=GPIO%d B=GPIO%d OK=GPIO%d BACK=GPIO%d)",
        (int)BOARD_PIN_ENCODER_A,
        (int)BOARD_PIN_ENCODER_B,
        (int)BOARD_PIN_ENCODER_BTN,
        (int)BOARD_PIN_BUTTON_KEY);
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

/* Read the encoder and emit ONE axis per detent.
 *   back_held == false : Up/Down tagged HARDWARE  -> per-view ViewInputMode remap
 *                        makes it Up/Down in lists, Left/Right in horizontal bars.
 *   back_held == true  : Left/Right tagged KEYBOARD -> bypasses the remap, giving
 *                        raw horizontal input for genuine 2-D views. */
static void poll_encoder(FuriPubSub* pubsub, uint32_t* seq, bool back_held) {
    uint8_t state = (uint8_t)((gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO));
    if(state == s_enc_prev) return;

    s_enc_accum += s_qdec[(s_enc_prev << 2) | state];
    s_enc_prev = state;

    while(s_enc_accum >= ENC_COUNTS_PER_DETENT || s_enc_accum <= -ENC_COUNTS_PER_DETENT) {
        int dir;
        if(s_enc_accum >= ENC_COUNTS_PER_DETENT) {
            s_enc_accum -= ENC_COUNTS_PER_DETENT;
            dir = ENC_DIR;
        } else {
            s_enc_accum += ENC_COUNTS_PER_DETENT;
            dir = -ENC_DIR;
        }

        if(back_held) {
            /* Back-shift: raw Left/Right, KEYBOARD source bypasses the per-view remap. */
            s_back_shift_used = true;
            publish_tap(
                pubsub,
                dir > 0 ? InputKeyRight : InputKeyLeft,
                seq,
                INPUT_SEQUENCE_SOURCE_KEYBOARD);
        } else {
            /* Plain turn: single axis, HARDWARE source so the view dispatcher can
             * remap it per the focused view's ViewInputMode. */
            publish_tap(
                pubsub,
                dir > 0 ? InputKeyDown : InputKeyUp,
                seq,
                INPUT_SEQUENCE_SOURCE_HARDWARE);
        }
    }
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    if(!s_ready) return;
    const uint32_t now = furi_get_tick();
    const uint32_t long_ticks = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    const uint32_t repeat_ticks = furi_ms_to_ticks(INPUT_REPEAT_MS);

    /* Active-low: pressed == level 0 */
    bool raw_ok = (gpio_get_level(BTN_OK_GPIO) == 0);
    bool raw_bk = (gpio_get_level(BTN_BK_GPIO) == 0);

    /* ---- OK = encoder push (full Press/Long/Short/Repeat/Release stream) ---- */
    bool changed_ok, state_ok = false;
    changed_ok = debounce(&s_ok, raw_ok, now, &state_ok);
    if(changed_ok) {
        if(state_ok) {
            s_ok.press_started_at = now;
            s_ok.long_fired = false;
            publish(pubsub, InputKeyOk, InputTypePress, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
        } else {
            if(!s_ok.long_fired) {
                publish(
                    pubsub, InputKeyOk, InputTypeShort, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
            }
            publish(
                pubsub, InputKeyOk, InputTypeRelease, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
        }
    } else if(s_ok.pressed) {
        if(!s_ok.long_fired && (now - s_ok.press_started_at) >= long_ticks) {
            s_ok.long_fired = true;
            s_ok.press_started_at = now;
            publish(pubsub, InputKeyOk, InputTypeLong, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
        } else if(s_ok.long_fired && (now - s_ok.press_started_at) >= repeat_ticks) {
            s_ok.press_started_at = now;
            publish(
                pubsub, InputKeyOk, InputTypeRepeat, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
        }
    }

    /* ---- Back = dedicated button. A plain tap emits Back on release. Holding Back
     * turns the encoder into a Left/Right shift (poll_encoder); in that case the
     * tap is suppressed so the shift never navigates back. ---- */
    bool changed_bk, state_bk = false;
    changed_bk = debounce(&s_back, raw_bk, now, &state_bk);
    if(changed_bk) {
        if(state_bk) {
            /* pressed: begin a potential shift-hold */
            s_back_shift_used = false;
        } else {
            /* released: real Back only if no turn happened during the hold */
            if(!s_back_shift_used) {
                publish_tap(pubsub, InputKeyBack, sequence_counter, INPUT_SEQUENCE_SOURCE_HARDWARE);
            }
            s_back_shift_used = false;
        }
    }

    /* Encoder last, using Back's debounced state as the shift modifier. */
    poll_encoder(pubsub, sequence_counter, s_back.pressed);
}
