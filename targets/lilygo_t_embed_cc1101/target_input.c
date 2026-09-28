/**
 * target_input.c — LilyGO T-Embed CC1101 rotary-encoder input driver
 *
 * Hardware: an EC11-style rotary encoder plus two momentary buttons, all wired to
 * GND and read with internal pull-ups (pressed / active == level 0).
 *
 *   Encoder A/B (GPIO4 / GPIO5): quadrature rotation.
 *   Encoder push (GPIO0)       : Ok, and — held while turning — the Left/Right modifier.
 *   Back button  (GPIO6)       : Back.
 *
 * Nav model — SINGLE-AXIS with an Ok-held modifier (matches components/gui/modules/
 * menu.c, which walks the grid linearly on Up/Down and jumps columns on Left/Right):
 *   turn                 -> Up / Down      (scroll lists, walk the menu, move items)
 *   hold Ok (push) + turn-> Left / Right   (change a setting's value, move an action
 *                                           bar, move the keyboard cursor horizontally)
 * Plain turning therefore NEVER changes a setting value or moves diagonally — a bare
 * turn is always a clean single step. The Ok press stream is deferred to release so a
 * modifier turn can never leak an Ok tap.
 *
 * Freeze escape hatch: hold Ok + Back together for REBOOT_COMBO_MS -> esp_restart().
 * Works whenever the input service is still scheduled (a stuck UI / service); a total
 * scheduler hang is caught by the task/interrupt watchdogs instead.
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
#include <esp_system.h>
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
/* Hold Ok + Back together this long to force a reboot (freeze escape hatch). */
#define REBOOT_COMBO_MS     2000U

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

typedef struct {
    bool pressed; /* debounced logical state */
    bool raw_last; /* last raw sample */
    uint32_t changed_at; /* tick of last raw change (debounce) */
    uint32_t press_started_at;
    bool long_fired; /* long threshold already handled this hold */
} BtnState;

static BtnState s_ok;
static BtnState s_back;

/* Set when the encoder turned while Ok was held (this hold was a Left/Right
 * modifier, so suppress the Ok tap/long on release). Cleared on each Ok press. */
static bool s_ok_mod_used;

/* Ok+Back reboot combo timing. */
static bool s_combo_armed;
static uint32_t s_combo_started_at;

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
    s_ok_mod_used = false;
    s_combo_armed = false;
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

/* Read the encoder. A plain detent emits Up/Down; if Ok (the encoder push) is held
 * it emits Left/Right instead — the value-change / horizontal modifier. */
static void poll_encoder(FuriPubSub* pubsub, uint32_t* seq, bool ok_held) {
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

        if(ok_held) {
            /* Ok-held modifier: horizontal. Mark the hold so the Ok tap is suppressed. */
            s_ok_mod_used = true;
            publish_tap(pubsub, dir > 0 ? InputKeyRight : InputKeyLeft, seq);
        } else {
            publish_tap(pubsub, dir > 0 ? InputKeyDown : InputKeyUp, seq);
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

    /* ---- Freeze escape hatch: Ok + Back held together -> reboot. Uses raw levels
     *      so it works regardless of debounce/UI state, as long as this poll runs. ---- */
    if(raw_ok && raw_bk) {
        if(!s_combo_armed) {
            s_combo_armed = true;
            s_combo_started_at = now;
        } else if((now - s_combo_started_at) >= furi_ms_to_ticks(REBOOT_COMBO_MS)) {
            FURI_LOG_W(TAG, "Ok+Back reboot combo -> esp_restart()");
            esp_restart();
        }
    } else {
        s_combo_armed = false;
    }

    /* Encoder: plain turn = Up/Down; Ok-held + turn = Left/Right. */
    poll_encoder(pubsub, sequence_counter, raw_ok);

    /* ---- OK = encoder push. A plain tap = Ok; held + a turn = the Left/Right
     *      modifier (see poll_encoder). The press stream is DEFERRED to release so a
     *      modifier turn never leaks an Ok Press/Short. ---- */
    bool changed_ok, state_ok = false;
    changed_ok = debounce(&s_ok, raw_ok, now, &state_ok);
    if(changed_ok) {
        if(state_ok) {
            /* pressed: start timing, defer the event until we know if it's a modifier */
            s_ok.press_started_at = now;
            s_ok.long_fired = false;
            s_ok_mod_used = false;
        } else {
            /* released */
            if(s_ok_mod_used) {
                /* this hold was a Left/Right modifier — emit no Ok event */
            } else if(s_ok.long_fired) {
                publish(pubsub, InputKeyOk, InputTypeRelease, sequence_counter);
            } else {
                publish_tap(pubsub, InputKeyOk, sequence_counter);
            }
        }
    } else if(s_ok.pressed && !s_ok_mod_used) {
        if(!s_ok.long_fired && (now - s_ok.press_started_at) >= long_ticks) {
            s_ok.long_fired = true;
            s_ok.press_started_at = now;
            publish(pubsub, InputKeyOk, InputTypePress, sequence_counter);
            publish(pubsub, InputKeyOk, InputTypeLong, sequence_counter);
        } else if(s_ok.long_fired && (now - s_ok.press_started_at) >= repeat_ticks) {
            s_ok.press_started_at = now;
            publish(pubsub, InputKeyOk, InputTypeRepeat, sequence_counter);
        }
    }

    /* ---- Back = dedicated button (tap = Back; a long hold fires Back once) ---- */
    bool changed_bk, state_bk = false;
    changed_bk = debounce(&s_back, raw_bk, now, &state_bk);
    if(changed_bk) {
        if(state_bk) {
            s_back.press_started_at = now;
            s_back.long_fired = false;
        } else {
            if(!s_back.long_fired) {
                publish_tap(pubsub, InputKeyBack, sequence_counter);
            }
        }
    } else if(s_back.pressed && !s_back.long_fired &&
              (now - s_back.press_started_at) >= long_ticks) {
        s_back.long_fired = true;
        publish_tap(pubsub, InputKeyBack, sequence_counter);
    }
}
