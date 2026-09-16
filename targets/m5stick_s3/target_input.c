/**
 * target_input.c — M5Stack StickS3 two-button input driver
 *
 * Two physical buttons (KEY1=GPIO11 "M5"/front, KEY2=GPIO12 side). Control layout tuned
 * for easy list navigation on a 2-button device (the side button is pure nav with
 * hold-to-fast-scroll; Back lives on the front button so the side button's hold is free):
 *
 *   KEY1 (M5/front): tap               -> InputKeyOk   (Select)
 *                    hold >=600ms      -> InputKeyBack (SHORT — pops a scene)
 *                    keep holding >=1200ms -> InputKeyBack (LONG — game exit, e.g. Snake)
 *   KEY2 (side):     single-tap        -> InputKeyDown (one step)
 *                    HOLD >=320ms      -> InputKeyDown auto-repeats ~9/s (fast scroll)
 *                    double-tap        -> InputKeyUp   (2nd tap within 200ms)
 *   Screen wake:     any deliberate BMI270 movement wakes the screen once it has
 *                    gone dark from inactivity — and does NOTHING else, ever. Tilt is
 *                    wake-only: it never injects navigation. Honors the s_tilt_enabled
 *                    toggle (System settings).
 *
 * A single-tap Down is confirmed only after the 200ms double-tap window closes, so it has
 * a small deliberate lag to distinguish it from a double-tap Up; a HOLD bypasses that and
 * fast-scrolls immediately. Ok fires on release so a hold can become Back instead.
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

/* Control scheme (tuned for 2-button list navigation):
 *   KEY1 (GPIO11, "M5"/front button) = tap Select/Ok, hold Back (>=600ms short, >=1200ms
 *                                      long for game-exit). Ok fires on release so the
 *                                      same button's hold can become Back.
 *   KEY2 (GPIO12, side button)       = tap Down, HOLD auto-scroll Down (fast list scroll),
 *                                      double-tap Up. No Back here — its hold is the
 *                                      fast-scroll gesture now.
 * The BMI270 IMU wakes the screen from inactivity and does nothing more — it never injects
 * navigation keys (tilt is wake-only, by request). */
#define DW_DOUBLE_MS     200U  /* max gap first-release -> second-release for a double (snappier Down) */
#define DW_LONG_MS       600U  /* hold KEY1 -> Back (short, pops a scene) */
#define DW_LONG2_MS      1200U /* keep holding KEY1 -> long Back (game exit) */
#define SCROLL_HOLD_MS   320U  /* hold KEY2 this long -> enter fast auto-scroll (Down) */
#define SCROLL_REPEAT_MS 110U  /* auto-scroll step interval while KEY2 held */
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
    /* KEY2 (side) single-vs-double click tracking */
    bool waiting; /* first click released, awaiting a possible second click */
    uint32_t first_release_at;
    /* KEY1 (front) hold-to-Back tracking */
    bool long_fired; /* short Back already emitted this hold */
    bool long2_fired; /* long Back already emitted this hold */
    /* KEY2 (side) hold-to-fast-scroll tracking */
    bool scroll_active; /* currently auto-scrolling Down while held */
    uint32_t last_scroll_at; /* tick of last auto-scroll step */
} BtnState;

static BtnState s_key1;
static BtnState s_key2;

static bool s_ready = false;

/* ---- BMI270 motion -> wake the screen (only; never navigation) ---- */
static volatile bool s_tilt_enabled = true; /* default on; System settings persists it */
/* Runtime tilt-nav suppression (SubGhz RX capture views): tilt does nothing while
 * capturing, so an accidental roll can't open the freq menu. Wake path unaffected. */
static volatile bool s_tilt_nav_suppressed = false;

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
/* Tilt -> Left/Right nav latch: set when a roll fires a tap (or when a wake gesture
 * fires), cleared only when the roll returns inside the release band. That single
 * latch IS the hysteresis: a held tilt yields ONE tap, and a wake roll can't also
 * navigate until it recenters. */
static bool s_tilt_latched = false;

void target_input_set_tilt_enabled(bool enabled) {
    s_tilt_enabled = enabled;
    if(!enabled) s_acc_base_have = false; /* re-seed the baseline next time it's on */
    s_tilt_latched = false; /* drop any pending tilt latch when the toggle changes */
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

void target_input_set_tilt_nav_suppressed(bool suppressed) {
    s_tilt_nav_suppressed = suppressed;
    /* Drop any pending roll latch so a held tilt can't fire on un-suppress. */
    s_tilt_latched = false;
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

/* ---- BMI270 tilt -> Left/Right navigation (re-added by request, L/R ONLY) ----
 * The 2-button scheme owns Up/Down/Ok/Back; Left/Right are otherwise unreachable,
 * which breaks any UI that needs a left/right tap (exit-save, emulate, char pickers).
 * Rolling the stick left/right past TILT_FIRE_LSB emits ONE InputKeyLeft/Right tap;
 * the roll must fall back under TILT_RELEASE_LSB (return-to-center) before it can fire
 * again, so a HELD tilt is a single tap, not a stream. Runs only while the screen is
 * LIT, and a wake gesture arms the same latch, so tilt-nav never fights tilt-to-wake
 * (which fires only while dark). Gated by s_tilt_enabled at the call site.
 *
 * At +/-2g, 1g == 16384 LSB. Threshold the roll axis directly (integer, matching
 * motion_wake_edge's style): ~35 deg roll ~= sin(35)*16384 ~= 9400 LSB (fire),
 * ~25 deg ~= 6900 LSB (release; the gap is the hysteresis). Axis/sign are mount-
 * dependent -> exposed as one-line tunables (design doc 2026-07-28-bmi270-tilt-nav). */
#define TILT_FIRE_LSB           9400
#define TILT_RELEASE_LSB        6900
#define TILT_ROLL_AXIS(x, y, z) (x) /* raw axis that swings with left/right roll */
#define TILT_INVERT             1    /* set 1 if Left/Right come out swapped on-device */

/* Returns InputKeyLeft/InputKeyRight to emit THIS sample, or InputKeyMAX for none
 * (InputKeyUp==0, so MAX is the 'none' sentinel). Pure per-sample edge detector over
 * s_tilt_latched, mirroring motion_wake_edge(). */
static InputKey tilt_nav_edge(int16_t x, int16_t y, int16_t z, uint32_t now) {
    (void)now; /* wide FIRE/RELEASE band + 40ms sampling => no time refractory needed */
    (void)x;
    (void)y;
    (void)z; /* only the TILT_ROLL_AXIS one is used; void all so the axis is a 1-line swap */
    int32_t roll = TILT_ROLL_AXIS(x, y, z);
    int32_t mag = roll < 0 ? -roll : roll;

    if(s_tilt_latched) {
        /* already fired (or woke) on this roll -> wait for return-to-center to re-arm */
        if(mag < TILT_RELEASE_LSB) s_tilt_latched = false;
        return InputKeyMAX;
    }
    if(mag >= TILT_FIRE_LSB) {
        s_tilt_latched = true;
        bool right = (roll >= 0);
#if TILT_INVERT
        right = !right;
#endif
        return right ? InputKeyRight : InputKeyLeft;
    }
    return InputKeyMAX;
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
    const uint32_t scroll_hold_ticks = furi_ms_to_ticks(SCROLL_HOLD_MS);
    const uint32_t scroll_repeat_ticks = furi_ms_to_ticks(SCROLL_REPEAT_MS);

    /* Active-low: pressed == level 0 */
    bool raw1 = (gpio_get_level(BTN_KEY1_GPIO) == 0);
    bool raw2 = (gpio_get_level(BTN_KEY2_GPIO) == 0);

    /* ---- KEY1 (M5/front) = tap Select/Ok, hold Back ---- */
    bool changed1, state1 = false;
    changed1 = debounce(&s_key1, raw1, now, &state1);
    if(changed1) {
        if(state1) {
            /* press edge */
            s_key1.press_started_at = now;
            s_key1.long_fired = false;
            s_key1.long2_fired = false;
        } else if(!s_key1.long_fired) {
            /* short release (never became a hold) -> Ok */
            publish_tap(pubsub, InputKeyOk, sequence_counter);
        }
        /* release after a hold-Back -> nothing more */
    } else if(s_key1.pressed) {
        uint32_t held = now - s_key1.press_started_at;
        if(!s_key1.long_fired && held >= long_ticks) {
            /* held past the long threshold -> Back (short, pops a scene) */
            s_key1.long_fired = true;
            publish_tap(pubsub, InputKeyBack, sequence_counter);
        } else if(s_key1.long_fired && !s_key1.long2_fired && held >= long2_ticks) {
            /* keep holding -> long Back, for games that gate exit on InputTypeLong */
            s_key1.long2_fired = true;
            publish_long(pubsub, InputKeyBack, sequence_counter);
        }
    }

    /* ---- KEY2 (side) = tap Down / hold fast-scroll Down / double-tap Up ---- */
    bool changed2, state2 = false;
    changed2 = debounce(&s_key2, raw2, now, &state2);
    if(changed2) {
        if(state2) {
            /* press edge */
            s_key2.press_started_at = now;
            s_key2.scroll_active = false;
        } else {
            /* release */
            if(s_key2.scroll_active) {
                /* was fast-scrolling -> just stop (no trailing Down, no double window) */
                s_key2.scroll_active = false;
                s_key2.waiting = false;
            } else if(s_key2.waiting && (now - s_key2.first_release_at) <= double_ticks) {
                /* second quick click within the window -> double -> Up */
                publish_tap(pubsub, InputKeyUp, sequence_counter);
                s_key2.waiting = false;
            } else {
                /* first click -> open the double-click window */
                s_key2.waiting = true;
                s_key2.first_release_at = now;
            }
        }
    } else if(s_key2.pressed) {
        /* held without release: after SCROLL_HOLD_MS, auto-scroll Down and keep repeating */
        uint32_t held = now - s_key2.press_started_at;
        if(!s_key2.scroll_active && held >= scroll_hold_ticks) {
            s_key2.scroll_active = true;
            s_key2.waiting = false; /* a hold is not a tap */
            s_key2.last_scroll_at = now;
            publish_tap(pubsub, InputKeyDown, sequence_counter);
        } else if(s_key2.scroll_active && (now - s_key2.last_scroll_at) >= scroll_repeat_ticks) {
            s_key2.last_scroll_at = now;
            publish_tap(pubsub, InputKeyDown, sequence_counter);
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
            /* motion_wake_edge() runs on every sample so its baseline keeps chasing the
             * resting pose; a wake is only EMITTED while the screen is dark. */
            bool woke = motion_wake_edge(ax, ay, az, now);
            if(woke && dark) {
                /* Screen is dark: emit one wake. A bare Press+Release (no Short/Long)
                 * turns the backlight back on via the notification service and resets
                 * the desktop idle timer, but no view treats it as a keypress. */
                publish(pubsub, WAKE_KEY, InputTypePress, sequence_counter);
                publish(pubsub, WAKE_KEY, InputTypeRelease, sequence_counter);
                s_tilt_latched = true; /* the wake roll must recenter before it can navigate */
            }
            /* Tilt -> Left/Right ONLY while the screen is lit, so it never fights the
             * dark-only wake gesture and a wake roll never bleeds into a nav tap.
             * Up/Down stay button-only, per request. */
            if(!dark && !s_tilt_nav_suppressed) {
                InputKey tk = tilt_nav_edge(ax, ay, az, now);
                if(tk != InputKeyMAX) publish_tap(pubsub, tk, sequence_counter);
            }
        }
    }
}
