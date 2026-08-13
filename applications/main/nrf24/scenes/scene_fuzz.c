/* nRF24 Fuzzer -- attack tooling, PRIVATE builds only (whole file gated,
 * same pattern as scene_sniffer.c / subghz_scene_garage.c).
 *
 * Mutates a base frame with Task 7's nrf24_fuzz_next() and blasts each
 * mutation at a fixed target address/channel via Task 2's nrf24_tx_raw().
 * This is a dumb send-and-count loop: it has no way to observe a target's
 * reaction, so it never claims a crash/vuln verdict -- only "N frames sent"
 * (see the on-screen header text, verbatim per the brief).
 *
 * Base frame: most recent nrf24_capture entry if the store is non-empty,
 * else a default 16-byte incrementing pattern (0x00..0x0F) so the scene
 * works standalone before any capture has been made. Target addr/channel
 * are derived from that same capture (or a default E7E7E7E7E7 / ch40) --
 * this is intentionally the simplest correct v1: no byte-level address
 * editor, that's out of scope here.
 *
 * Two UI states (scene_manager_set_scene_state), reusing scene_sniffer.c's
 * worker/abort/safe-join shape:
 *   0 Setup   - Widget: mode name + Left/Right cycle mode, Center=Start
 *   1 Running - Widget: on-air indicator + live step/sent counters,
 *               Back aborts (stop flag + join, same as Sniffer's Replaying)
 */
#ifndef PUBLIC_BUILD

#include "../nrf24_app.h"
#include "../helpers/nrf24_capture.h"
#include "../helpers/nrf24_fuzz.h"

#include <furi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_TX_DELAY_MS 20 /* per-iteration loop-delay ceiling; real on-air rate
                             * is lower -- nrf24_tx_raw re-inits the radio each call */
#define FUZZ_MODE_COUNT  4 /* N24F_BITFLIP..N24F_RANDOM */

typedef enum {
    FuzzStateSetup = 0,
    FuzzStateRunning = 1,
} FuzzState;

#define FUZZ_EVT_MODE_PREV 1
#define FUZZ_EVT_MODE_NEXT 2
#define FUZZ_EVT_START     3

static const char* const FUZZ_MODE_NAMES[FUZZ_MODE_COUNT] = {
    "BitFlip",
    "LenSweep",
    "Field",
    "Random",
};

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop; /* Back-abort request while Running */
    volatile bool done; /* set by the worker when its loop exits */
    volatile uint32_t step;
    volatile uint32_t sent;
    int mode; /* Nrf24FuzzMode, only touched from the UI thread while Setup
               * is shown -- never mutated once a worker is running */
    uint32_t seed;
    uint8_t base[32];
    uint8_t base_len;
    uint8_t target_addr[5];
    uint8_t target_chan;
} FuzzCtx;

static FuzzCtx* g_ctx = NULL;

/* ---- Worker: mutate + TX, bounded rate, stop-flag abort ---- */
static int32_t fuzz_worker(void* context) {
    FuzzCtx* ctx = context;
    uint8_t out[32];
    uint8_t out_len = 0;

    while(!ctx->stop) {
        nrf24_fuzz_next(
            ctx->base, ctx->base_len, (Nrf24FuzzMode)ctx->mode, ctx->seed, ctx->step, out, &out_len);
        nrf24_tx_raw(ctx->target_addr, ctx->target_chan, out, out_len, 1);
        ctx->step++;
        ctx->sent++;
        furi_delay_ms(FUZZ_TX_DELAY_MS);
    }

    ctx->done = true;
    return 0;
}

/* ---- Widget button -> custom event plumbing (scene_sniffer.c pattern) ---- */
static void fuzz_widget_button_cb(GuiButtonType result, InputType type, void* context) {
    Nrf24App* app = context;
    if(type != InputTypeShort) return;
    uint32_t evt = 0;
    if(result == GuiButtonTypeLeft) evt = FUZZ_EVT_MODE_PREV;
    else if(result == GuiButtonTypeRight) evt = FUZZ_EVT_MODE_NEXT;
    else if(result == GuiButtonTypeCenter) evt = FUZZ_EVT_START;
    if(evt) view_dispatcher_send_custom_event(app->view_dispatcher, evt);
}

/* ---- View builders (do not switch views themselves -- callers do, same
 * convention as sniffer_build_scanning_widget()) ---- */
static void fuzz_build_setup_widget(Nrf24App* app, FuzzCtx* ctx) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 12, AlignCenter, AlignTop, "\e#nRF24 Fuzzer\e#", false);
    widget_add_text_box_element(
        app->widget,
        0,
        12,
        128,
        24,
        AlignCenter,
        AlignTop,
        "Effect is target-dependent -- reports what it sent, not a crash verdict.",
        false);

    char line1[32];
    snprintf(line1, sizeof(line1), "Mode: %s", FUZZ_MODE_NAMES[ctx->mode]);
    widget_add_string_element(app->widget, 64, 38, AlignCenter, AlignTop, FontSecondary, line1);

    char line2[32];
    snprintf(line2, sizeof(line2), "ch%u  %ub base", ctx->target_chan, ctx->base_len);
    widget_add_string_element(app->widget, 64, 48, AlignCenter, AlignTop, FontSecondary, line2);

    widget_add_button_element(app->widget, GuiButtonTypeLeft, "-", fuzz_widget_button_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeCenter, "Start", fuzz_widget_button_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "+", fuzz_widget_button_cb, app);
}

static void fuzz_build_running_widget(Nrf24App* app, FuzzCtx* ctx) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 12, AlignCenter, AlignTop, "\e#TX on air...\e#", false);

    char l1[32];
    snprintf(l1, sizeof(l1), "Mode: %s", FUZZ_MODE_NAMES[ctx->mode]);
    widget_add_string_element(app->widget, 64, 20, AlignCenter, AlignTop, FontSecondary, l1);

    char l2[32];
    snprintf(l2, sizeof(l2), "Step %lu", (unsigned long)ctx->step);
    widget_add_string_element(app->widget, 64, 32, AlignCenter, AlignTop, FontSecondary, l2);

    char l3[32];
    snprintf(l3, sizeof(l3), "Sent %lu", (unsigned long)ctx->sent);
    widget_add_string_element(app->widget, 64, 44, AlignCenter, AlignTop, FontSecondary, l3);
}

/* ---- Scene lifecycle ---- */
void nrf24_app_scene_fuzz_on_enter(void* context) {
    Nrf24App* app = context;

    FuzzCtx* ctx = malloc(sizeof(FuzzCtx));
    if(!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;
    ctx->mode = N24F_BITFLIP;

    /* Base frame + target: most recent capture if the store is non-empty,
     * else a default 16-byte incrementing pattern / default target. */
    size_t n = nrf24_capture_count();
    if(n > 0) {
        const Nrf24Capture* c = nrf24_capture_get(n - 1);
        if(c) {
            uint8_t len = c->len;
            if(len == 0) len = 1;
            if(len > 32) len = 32;
            memcpy(ctx->base, c->payload, len);
            ctx->base_len = len;
            memcpy(ctx->target_addr, c->addr, sizeof(ctx->target_addr));
            ctx->target_chan = c->channel;
        }
    }
    if(ctx->base_len == 0) {
        for(int i = 0; i < 16; i++) ctx->base[i] = (uint8_t)i;
        ctx->base_len = 16;
        static const uint8_t def_addr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
        memcpy(ctx->target_addr, def_addr, sizeof(ctx->target_addr));
        ctx->target_chan = 40;
    }

    g_ctx = ctx;

    scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneFuzz, FuzzStateSetup);
    fuzz_build_setup_widget(app, ctx);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
}

bool nrf24_app_scene_fuzz_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(!g_ctx) return false;

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, Nrf24AppSceneFuzz);

    if(event.type == SceneManagerEventTypeTick) {
        if(state == FuzzStateRunning) {
            if(g_ctx->done) {
                if(g_ctx->worker) {
                    furi_thread_join(g_ctx->worker);
                    furi_thread_free(g_ctx->worker);
                    g_ctx->worker = NULL;
                }
                scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneFuzz, FuzzStateSetup);
                fuzz_build_setup_widget(app, g_ctx);
            } else {
                fuzz_build_running_widget(app, g_ctx);
            }
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == FuzzStateSetup) {
            if(event.event == FUZZ_EVT_MODE_PREV) {
                g_ctx->mode = (g_ctx->mode + FUZZ_MODE_COUNT - 1) % FUZZ_MODE_COUNT;
                fuzz_build_setup_widget(app, g_ctx);
                return true;
            }
            if(event.event == FUZZ_EVT_MODE_NEXT) {
                g_ctx->mode = (g_ctx->mode + 1) % FUZZ_MODE_COUNT;
                fuzz_build_setup_widget(app, g_ctx);
                return true;
            }
            if(event.event == FUZZ_EVT_START) {
                g_ctx->stop = false;
                g_ctx->done = false;
                g_ctx->step = 0;
                g_ctx->sent = 0;
                g_ctx->seed = furi_get_tick();
                scene_manager_set_scene_state(
                    app->scene_manager, Nrf24AppSceneFuzz, FuzzStateRunning);
                fuzz_build_running_widget(app, g_ctx);

                g_ctx->worker = furi_thread_alloc_ex("Nrf24Fuzz", 4096, fuzz_worker, g_ctx);
                furi_thread_start(g_ctx->worker);
                return true;
            }
        }
        return false;
    }

    if(event.type == SceneManagerEventTypeBack) {
        if(state == FuzzStateRunning) {
            /* Abort the in-flight loop: the Tick handler above transitions
             * back to Setup once fuzz_worker() sets done (on_exit()'s
             * join is the safety net either way, same as scene_sniffer.c). */
            g_ctx->stop = true;
            return true;
        }
        /* Setup: let scene_manager pop to the app menu. */
        return false;
    }

    return false;
}

void nrf24_app_scene_fuzz_on_exit(void* context) {
    Nrf24App* app = context;
    if(!g_ctx) return;

    g_ctx->stop = true;
    if(g_ctx->worker) {
        furi_thread_join(g_ctx->worker);
        furi_thread_free(g_ctx->worker);
        g_ctx->worker = NULL;
    }
    free(g_ctx);
    g_ctx = NULL;

    widget_reset(app->widget);
}

#endif /* !PUBLIC_BUILD -- attack tooling, private only */
