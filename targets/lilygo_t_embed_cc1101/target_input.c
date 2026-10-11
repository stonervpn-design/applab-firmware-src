/**
 * target_input.c — LilyGO T-Embed CC1101 rotary-encoder input driver
 *
 * Hardware: an EC11-style rotary encoder plus two momentary buttons, all wired to
 * GND and read with internal pull-ups (pressed / active == level 0).
 *
 *   Encoder A/B (GPIO4 / GPIO5): quadrature rotation -> hardware PCNT peripheral.
 *   Encoder push (GPIO0)       : Ok.
 *   Back button  (GPIO6)       : Back.
 *
 * Nav model — one knob, two axes via a modifier:
 *   - a plain turn is Up/Down (CW = Down, CCW = Up): menus, keyboard and settings
 *     all walk linearly on Up/Down, and Ok selects / toggles editing;
 *   - HOLD Ok (the encoder push) and turn for Left/Right (CW = Right, CCW = Left).
 *     This reaches the horizontal affordances some apps bind to Left/Right — e.g.
 *     ProtoPirate's receive screen opens "Config" on Left and deletes on Right.
 * When a turn happens while Ok is held, that Ok press is consumed as the modifier:
 * no Ok Short/Long/Repeat is emitted for it (so you don't also trigger "select").
 *
 * Why PCNT (and not software polling):
 *   The encoder is decoded by the ESP32-S3 PCNT (pulse-counter) peripheral in x4
 *   quadrature mode, with its hardware glitch filter enabled. PCNT latches EVERY
 *   A/B edge in hardware, so no transition is ever missed regardless of how fast
 *   the knob is spun or how coarsely we poll — and the glitch filter rejects
 *   contact bounce. The old software decoder sampled A/B every 4 ms and lost any
 *   edge that fell between samples (and mis-scored bounce), which made the UI
 *   "stick" for several detents and then jump several rows at once. PCNT fixes
 *   that at the source: the poll just reads the accumulated hardware count and
 *   converts whole detents into Up/Down taps.
 *
 * Freeze escape hatch: hold Ok + Back together for REBOOT_COMBO_MS -> esp_restart().
 * Works whenever the input service is still scheduled (a stuck UI / service); a total
 * scheduler hang is caught by the task/interrupt watchdogs instead.
 *
 * Called every INPUT_POLL_MS (4 ms) from the input service. The poll cadence no
 * longer gates accuracy (PCNT counts in hardware); it only bounds input latency.
 *
 * Flip ENC_DIR if scrolling goes the wrong way; adjust ENC_COUNTS_PER_DETENT
 * (4 for x4 decoding; 2 if an EC11 emits half-steps per click) if one physical
 * click scrolls by the wrong number of steps.
 */

#include "target_input.h"

#include <furi.h>
#include <input.h>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <esp_system.h>
#include "boards/board.h"

#define TAG "InputTEmbed"

#define ENC_A_GPIO   ((gpio_num_t)BOARD_PIN_ENCODER_A)
#define ENC_B_GPIO   ((gpio_num_t)BOARD_PIN_ENCODER_B)
#define BTN_OK_GPIO  ((gpio_num_t)BOARD_PIN_ENCODER_BTN)
#define BTN_BK_GPIO  ((gpio_num_t)BOARD_PIN_BUTTON_KEY)

/* +1 = CW scrolls Down; flip to -1 if the encoder scrolls the wrong way. */
#define ENC_DIR                 (+1)
/* Quadrature counts per physical detent (x4 decoding: 4; some EC11s: 2). */
#define ENC_COUNTS_PER_DETENT   4

/* PCNT counter limits. Kept well clear of the per-poll travel so the hardware
 * counter never auto-zeros mid-turn; we re-center it in software long before. */
#define ENC_PCNT_HIGH_LIMIT     1000
#define ENC_PCNT_LOW_LIMIT      (-1000)
#define ENC_PCNT_RECENTER       512
/* Hardware debounce for the A/B contacts (ns). PCNT drops pulses shorter than this. */
#define ENC_GLITCH_NS           1000U

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

/* Set when the current Ok hold has been used as a Left/Right turn-modifier, so its
 * release does not also emit an Ok Short (and Long/Repeat are suppressed). */
static bool s_ok_rot_mod;

/* Ok+Back reboot combo timing. */
static bool s_combo_armed;
static uint32_t s_combo_started_at;

/* PCNT quadrature decoder state */
static pcnt_unit_handle_t s_enc_pcnt;
static int s_enc_last; /* last raw hardware count read */
static int8_t s_enc_accum; /* signed sub-step remainder toward the next detent */

static bool s_ready = false;

void target_input_init(void) {
    /* Buttons (Ok = encoder push, Back) are plain GPIO inputs with pull-ups.
     * The encoder A/B lines are owned by the PCNT peripheral, configured below. */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_ENCODER_BTN) | (1ULL << BOARD_PIN_BUTTON_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_ok = (BtnState){0};
    s_back = (BtnState){0};
    s_ok_rot_mod = false;
    s_combo_armed = false;
    s_enc_accum = 0;
    s_enc_last = 0;
    s_enc_pcnt = NULL;

    /* ---- PCNT: x4 quadrature decode of encoder A/B in hardware ----
     * Init is fail-soft on purpose: PCNT bring-up is extremely unlikely to fail,
     * but if it ever does we must NOT abort (that bootloops a flashed device).
     * We log, leave s_enc_pcnt NULL so poll_encoder() no-ops, and still bring up
     * the buttons + reboot combo so the device remains usable. */
    esp_err_t err;
    pcnt_unit_config_t unit_cfg = {
        .high_limit = ENC_PCNT_HIGH_LIMIT,
        .low_limit = ENC_PCNT_LOW_LIMIT,
    };
    pcnt_glitch_filter_config_t filter_cfg = {.max_glitch_ns = ENC_GLITCH_NS};
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num = BOARD_PIN_ENCODER_A,
        .level_gpio_num = BOARD_PIN_ENCODER_B,
    };
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num = BOARD_PIN_ENCODER_B,
        .level_gpio_num = BOARD_PIN_ENCODER_A,
    };
    pcnt_channel_handle_t chan_a = NULL;
    pcnt_channel_handle_t chan_b = NULL;

    do {
        pcnt_unit_handle_t unit = NULL;
        if((err = pcnt_new_unit(&unit_cfg, &unit)) != ESP_OK) break;
        if((err = pcnt_unit_set_glitch_filter(unit, &filter_cfg)) != ESP_OK) break;

        /* Channel A: edges on A, gated by the level of B. */
        if((err = pcnt_new_channel(unit, &chan_a_cfg, &chan_a)) != ESP_OK) break;
        if((err = pcnt_channel_set_edge_action(
                chan_a,
                PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                PCNT_CHANNEL_EDGE_ACTION_INCREASE)) != ESP_OK)
            break;
        if((err = pcnt_channel_set_level_action(
                chan_a,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK)
            break;

        /* Channel B: edges on B, gated by the level of A. */
        if((err = pcnt_new_channel(unit, &chan_b_cfg, &chan_b)) != ESP_OK) break;
        if((err = pcnt_channel_set_edge_action(
                chan_b,
                PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                PCNT_CHANNEL_EDGE_ACTION_DECREASE)) != ESP_OK)
            break;
        if((err = pcnt_channel_set_level_action(
                chan_b,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK)
            break;

        /* EC11 commons tie to GND -> internal pull-ups on A/B. */
        gpio_set_pull_mode(ENC_A_GPIO, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(ENC_B_GPIO, GPIO_PULLUP_ONLY);

        if((err = pcnt_unit_enable(unit)) != ESP_OK) break;
        if((err = pcnt_unit_clear_count(unit)) != ESP_OK) break;
        if((err = pcnt_unit_start(unit)) != ESP_OK) break;

        s_enc_pcnt = unit; /* publish only once fully configured */
    } while(0);

    if(s_enc_pcnt == NULL) {
        FURI_LOG_E(TAG, "PCNT encoder init failed (0x%x) - encoder disabled", (int)err);
    }

    s_ready = true;
    FURI_LOG_I(
        TAG,
        "T-Embed input init, PCNT encoder %s (A=GPIO%d B=GPIO%d OK=GPIO%d BACK=GPIO%d)",
        s_enc_pcnt ? "ok" : "DISABLED",
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

/* Read the hardware pulse count and emit one tap per whole detent. With Ok held
 * (ok_held), a detent is Left/Right (the turn-modifier); otherwise it is Up/Down. */
static void poll_encoder(FuriPubSub* pubsub, uint32_t* seq, bool ok_held) {
    if(s_enc_pcnt == NULL) return; /* PCNT bring-up failed; encoder disabled */
    int count = 0;
    if(pcnt_unit_get_count(s_enc_pcnt, &count) != ESP_OK) return;

    int delta = count - s_enc_last;
    s_enc_last = count;

    if(delta != 0) {
        s_enc_accum += (int8_t)(ENC_DIR * delta);

        while(s_enc_accum >= ENC_COUNTS_PER_DETENT || s_enc_accum <= -ENC_COUNTS_PER_DETENT) {
            bool positive = (s_enc_accum >= ENC_COUNTS_PER_DETENT);
            if(positive) {
                s_enc_accum -= ENC_COUNTS_PER_DETENT;
            } else {
                s_enc_accum += ENC_COUNTS_PER_DETENT;
            }
            if(ok_held) {
                /* Ok held -> horizontal axis; consume the Ok press as the modifier. */
                s_ok_rot_mod = true;
                publish_tap(pubsub, positive ? InputKeyRight : InputKeyLeft, seq);
            } else {
                publish_tap(pubsub, positive ? InputKeyDown : InputKeyUp, seq);
            }
        }
    }

    /* Re-center the hardware counter well before it reaches a limit, so the
     * (count - last) delta never has to span a hardware auto-zero. */
    if(count >= ENC_PCNT_RECENTER || count <= -ENC_PCNT_RECENTER) {
        pcnt_unit_clear_count(s_enc_pcnt);
        s_enc_last = 0;
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

    /* Encoder: plain turn = Up/Down; turn while Ok is held = Left/Right. Uses the
     * debounced Ok state (and only before a long-press) as the modifier gate. */
    bool ok_held_modifier = s_ok.pressed && !s_ok.long_fired;
    poll_encoder(pubsub, sequence_counter, ok_held_modifier);

    /* ---- OK = encoder push (full Press/Long/Short/Repeat/Release stream) ----
     * If this hold got used as a Left/Right turn-modifier (s_ok_rot_mod), we emit
     * no Ok Short/Long/Repeat for it — only the balancing Press/Release. */
    bool changed_ok, state_ok = false;
    changed_ok = debounce(&s_ok, raw_ok, now, &state_ok);
    if(changed_ok) {
        if(state_ok) {
            s_ok.press_started_at = now;
            s_ok.long_fired = false;
            s_ok_rot_mod = false;
            publish(pubsub, InputKeyOk, InputTypePress, sequence_counter);
        } else {
            if(!s_ok.long_fired && !s_ok_rot_mod) {
                publish(pubsub, InputKeyOk, InputTypeShort, sequence_counter);
            }
            publish(pubsub, InputKeyOk, InputTypeRelease, sequence_counter);
        }
    } else if(s_ok.pressed && !s_ok_rot_mod) {
        if(!s_ok.long_fired && (now - s_ok.press_started_at) >= long_ticks) {
            s_ok.long_fired = true;
            s_ok.press_started_at = now;
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
