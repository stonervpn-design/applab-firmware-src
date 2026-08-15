/* nRF24 Fusion Spectrum -- full-screen RGB565 color takeover, PRIVATE builds
 * only (whole file gated, same pattern as scene_sniffer.c / scene_fuzz.c).
 *
 * Stage 1 of the Fusion Spectrum feature: occupancy-only. Two workers drive a
 * fused 126-channel model and a color heat-waterfall rendered straight to the
 * ST7789 panel, bypassing the Flipper 1-bit mono canvas:
 *
 *   - Sweep worker (FuriThread): the classic scene_spectrum.c loop -- per
 *     channel nrf24_hw_listen_rpd_dwell(ch,dwell), EMA into model.occ[] under
 *     a FuriMutex; max-hold peak when enabled; sweep_count++.
 *   - Render worker (FuriThread, ~20 FPS): snapshot the model under the mutex,
 *     push one fused waterfall row (nrf24_fuse_intensity), then draw UNLOCKED
 *     in horizontal STRIPES reusing ONE small MALLOC_CAP_DMA buffer (no full
 *     240x135x2 frame -- Cardputer has no PSRAM). Top half = heat-gradient
 *     bars (peak line + glow); bottom half = scrolling heat waterfall; a white
 *     cursor column + a 5x7 "<MHZ> <PCT>%" readout band overlay. Each stripe
 *     is byte-swapped for the ST7789 (via a pre-swapped heat LUT + swapped
 *     colour constants) and blitted with esp_lcd_panel_draw_bitmap while
 *     holding the shared SPI bus lock.
 *
 * Display takeover mirrors doom_app.c: gui_direct_draw_acquire() ->
 * furi_hal_display_get_panel_handle()/get_h_res()/get_v_res() ->
 * furi_hal_spi_bus_lock()+esp_lcd_panel_draw_bitmap()+unlock. The nRF24 sweep
 * (nrf24_hw_acquire == furi_hal_spi_acquire, which also takes the global bus
 * lock on this shared-bus board) and the panel blits therefore serialize on
 * the same SPI bus lock -- the exact coexistence the classic spectrum already
 * proves.
 *
 * Input: under direct-draw the GUI drops viewport input (gui.c: `direct_draw
 * && !ongoing_input_view_port` -> break), so -- like doom -- we subscribe to
 * RECORD_INPUT_EVENTS directly and route Left/Right/Back to view-dispatcher
 * custom events handled in on_event (Stage 1 controls = cursor L/R + exit;
 * the full control set lands in Stage 4).
 *
 * Wi-Fi/BLE overlays stay at -128 (no internal-radio worker) -- that fusion is
 * Stage 2/3. The klass byte is still packed into the waterfall for forward
 * compatibility but is not source-tinted in Stage 1.
 */
#ifndef PUBLIC_BUILD

#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_chanmap.h"
#include "../helpers/nrf24_heatmap.h"
#include "../helpers/nrf24_fusion_model.h"
#include "../helpers/nrf24_font5x7.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <furi_hal_spi_bus.h>
#include <gui/gui.h>
#include <input/input.h>

#include <esp_lcd_panel_ops.h>
#include <esp_heap_caps.h>

#include <string.h>
#include <stdio.h>

#define TAG "Nrf24Fusion"

#define WF_ROWS          90    /* waterfall history depth (126*90 ~= 11 KB) */
#define STRIPE_H         16    /* horizontal band height (one DMA blit) */
#define READOUT_H        9     /* top text band height (5x7 font + margin) */
#define FUSION_DWELL_US  250   /* per-channel RPD dwell (proven in classic) */
#define FUSION_OCC_THRESH 15   /* occ>=thresh with no Wi-Fi/BLE => SRC_HOPPER */

/* Compile-time RGB565 byte-swap (native -> ST7789 wire order). The stripe
 * buffer is entirely in ST7789 order, so every constant written into it is
 * pre-swapped; the heat LUT is pre-swapped too (see on_enter). 0x0000/0xFFFF
 * are swap-invariant. */
#define SW16(c) ((uint16_t)((((uint16_t)(c) >> 8) & 0x00FF) | (((uint16_t)(c) << 8) & 0xFF00)))
#define COL_BG    ((uint16_t)0x0000) /* black */
#define COL_WHITE ((uint16_t)0xFFFF) /* cursor + peak line */
#define COL_GLOW  SW16(0xFEB4)       /* pale warm halo above bar tops */

typedef enum {
    FusionEventCursorLeft = 100,
    FusionEventCursorRight,
    FusionEventExit,
} FusionEvent;

typedef struct {
    Nrf24App* app;

    FuriMutex* lock;         /* guards model */
    FusedSpectrum model;

    uint16_t heat_lut[256];  /* pre-swapped for ST7789 */
    Nrf24Waterfall wf;
    uint8_t* wf_backing;     /* NRF24_FUSION_CH * WF_ROWS, regular heap */

    uint16_t* stripe;        /* h_res * STRIPE_H u16, MALLOC_CAP_DMA */
    uint8_t*  xch;           /* h_res bytes: x-pixel -> channel index */

    esp_lcd_panel_handle_t panel;
    int h_res, v_res;

    volatile bool stop;
    volatile uint16_t dwell_us;
    volatile bool probe_ok;

    /* UI state (written by on_event on the view-dispatcher thread, read by the
     * render worker -- single writer, so plain volatile is enough) */
    volatile uint8_t cursor_ch;
    volatile bool    cursor_on;
    volatile bool    maxhold;
    volatile bool    freeze;

    FuriThread* sweep_worker;
    FuriThread* render_worker;

    FuriPubSub* input;
    FuriPubSubSubscription* input_sub;
} FusionCtx;

static FusionCtx* g_fusion_ctx = NULL;

/* ---------------------------------------------------------------- workers */

static int32_t fusion_sweep_worker(void* context) {
    FusionCtx* ctx = context;

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();
    ctx->probe_ok = ok;

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed; render runs empty");
        return 0;
    }

    uint8_t local[NRF24_FUSION_CH] = {0};

    while(!ctx->stop) {
        /* Per-channel SPI acquire (NOT across the whole sweep): the global
         * furi_hal_spi_bus_lock is shared with the render worker's blits and
         * the LCD commit; holding it across all 126 channels (~38ms) starves
         * the display. Release between channels so blits/commits interleave. */
        for(uint8_t ch = 0; ch < NRF24_FUSION_CH && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            uint8_t rpd = nrf24_hw_listen_rpd_dwell(ch, ctx->dwell_us);
            nrf24_hw_release();
            /* EMA into 0..100 occupancy (rpd is a 1-bit carrier flag) */
            local[ch] = (uint8_t)((local[ch] * 3 + rpd * 100) / 4);
        }

        furi_mutex_acquire(ctx->lock, FuriWaitForever);
        memcpy(ctx->model.occ, local, NRF24_FUSION_CH);
        ctx->model.sweep_count++;
        if(ctx->maxhold) {
            for(int i = 0; i < NRF24_FUSION_CH; i++)
                if(local[i] > ctx->model.peak[i]) ctx->model.peak[i] = local[i];
        }
        furi_mutex_release(ctx->lock);

        furi_delay_ms(2);
    }
    return 0;
}

static void fusion_render_frame(FusionCtx* ctx) {
    const int W = ctx->h_res;
    const int H = ctx->v_res;
    const int mid = H / 2;
    const int bar_span = (mid > READOUT_H) ? (mid - READOUT_H) : 1;
    uint16_t* buf = ctx->stripe;

    /* --- snapshot model + push a fresh fused waterfall row (under lock) --- */
    uint8_t fused[NRF24_FUSION_CH];
    uint8_t peak[NRF24_FUSION_CH];
    uint8_t wfrow[NRF24_FUSION_CH];

    const bool maxhold = ctx->maxhold;
    const bool frozen = ctx->freeze;

    furi_mutex_acquire(ctx->lock, FuriWaitForever);
    for(int ch = 0; ch < NRF24_FUSION_CH; ch++) {
        uint8_t occ = ctx->model.occ[ch];
        int8_t wdbm = ctx->model.wifi_dbm[ch];
        int8_t bdbm = ctx->model.ble_dbm[ch];
        uint8_t inten = nrf24_fuse_intensity(occ, wdbm, bdbm);
        fused[ch] = inten;
        peak[ch] = ctx->model.peak[ch];
        Nrf24SourceClass k = nrf24_classify(occ, wdbm, bdbm, FUSION_OCC_THRESH);
        ctx->model.klass[ch] = (uint8_t)k;
        uint8_t inten6 = (uint8_t)((inten * 63) / 100); /* low 6 bits */
        wfrow[ch] = (uint8_t)(inten6 | ((uint8_t)k << 6));
    }
    if(!frozen) nrf24_wf_push(&ctx->wf, wfrow);
    furi_mutex_release(ctx->lock);

    /* cursor column (channel -> x span) */
    const bool cursor_on = ctx->cursor_on;
    const uint8_t cur_ch = ctx->cursor_ch;
    int cur_x0 = (int)cur_ch * W / NRF24_FUSION_CH;
    int cur_x1 = ((int)cur_ch + 1) * W / NRF24_FUSION_CH;
    if(cur_x1 <= cur_x0) cur_x1 = cur_x0 + 1;

    /* --- draw bars + waterfall in stripes --- */
    for(int y0 = 0; y0 < H; y0 += STRIPE_H) {
        int sh = (y0 + STRIPE_H <= H) ? STRIPE_H : (H - y0);

        for(int i = 0; i < sh * W; i++) buf[i] = COL_BG;

        for(int ly = 0; ly < sh; ly++) {
            int y = y0 + ly;
            uint16_t* line = buf + (size_t)ly * W;

            if(y < mid) {
                /* top half: bars grow up from `mid` toward READOUT_H */
                if(y >= READOUT_H) {
                    for(int x = 0; x < W; x++) {
                        uint8_t ch = ctx->xch[x];
                        int bh = ((int)fused[ch] * bar_span) / 100;
                        int bar_top = mid - bh;
                        if(y >= bar_top) {
                            int frac = (mid - y) * 100 / bar_span; /* vertical gradient */
                            if(frac < 0) frac = 0;
                            if(frac > 100) frac = 100;
                            line[x] = nrf24_heat_color(ctx->heat_lut, (uint8_t)frac);
                        } else if(bh > 0 && y == bar_top - 1) {
                            line[x] = COL_GLOW; /* 1px halo above the bar */
                        }
                        if(maxhold) {
                            int ph = ((int)peak[ch] * bar_span) / 100;
                            if(ph > 0 && y == mid - ph) line[x] = COL_WHITE; /* peak line */
                        }
                    }
                }
            } else {
                /* bottom half: waterfall, newest row at y==mid, scrolling down */
                int age = y - mid;
                const uint8_t* wr = nrf24_wf_row(&ctx->wf, age);
                for(int x = 0; x < W; x++) {
                    uint8_t ch = ctx->xch[x];
                    uint8_t v6 = wr[ch] & 0x3F;
                    uint8_t inten = (uint8_t)((v6 * 100) / 63);
                    line[x] = nrf24_heat_color(ctx->heat_lut, inten);
                }
            }

            if(cursor_on) {
                for(int x = cur_x0; x < cur_x1 && x < W; x++) line[x] = COL_WHITE;
            }
        }

        furi_hal_spi_bus_lock();
        esp_lcd_panel_draw_bitmap(ctx->panel, 0, y0, W, y0 + sh, buf);
        furi_hal_spi_bus_unlock();
    }

    /* --- readout band (its own stripe blitted over the top) --- */
    for(int i = 0; i < READOUT_H * W; i++) buf[i] = COL_BG;
    char rd[24];
    snprintf(
        rd, sizeof(rd), "%uMHZ %u%%", (unsigned)nrf24_ch_to_mhz(cur_ch), (unsigned)fused[cur_ch]);
    nrf24_font5x7_draw(buf, W, READOUT_H, 2, 1, rd, COL_WHITE);
    furi_hal_spi_bus_lock();
    esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, W, READOUT_H, buf);
    furi_hal_spi_bus_unlock();
}

static int32_t fusion_render_worker(void* context) {
    FusionCtx* ctx = context;
    while(!ctx->stop) {
        fusion_render_frame(ctx);
        furi_delay_ms(50); /* ~15-20 FPS after draw time */
    }
    return 0;
}

/* ------------------------------------------------------------------ input */

static void fusion_input_callback(const void* value, void* context) {
    FusionCtx* ctx = context;
    const InputEvent* event = value;
    if(event->type != InputTypeShort) return;

    switch(event->key) {
    case InputKeyLeft:
        view_dispatcher_send_custom_event(ctx->app->view_dispatcher, FusionEventCursorLeft);
        break;
    case InputKeyRight:
        view_dispatcher_send_custom_event(ctx->app->view_dispatcher, FusionEventCursorRight);
        break;
    case InputKeyBack:
        view_dispatcher_send_custom_event(ctx->app->view_dispatcher, FusionEventExit);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- scene glue */

void nrf24_app_scene_fusion_spectrum_on_enter(void* context) {
    Nrf24App* app = context;
    g_fusion_ctx = NULL;

    FusionCtx* ctx = malloc(sizeof(FusionCtx));
    if(!ctx) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;
    ctx->dwell_us = FUSION_DWELL_US;

    bool dd_acquired = false;

    ctx->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!ctx->lock) goto fail;

    nrf24_fusion_model_reset(&ctx->model);

    /* pre-swap the heat palette once so per-pixel writes need no swap */
    uint16_t native_lut[256];
    nrf24_heat_build_lut(native_lut);
    for(int i = 0; i < 256; i++) ctx->heat_lut[i] = nrf24_rgb565_swap(native_lut[i]);

    ctx->wf_backing = malloc((size_t)NRF24_FUSION_CH * WF_ROWS);
    if(!ctx->wf_backing) goto fail;
    nrf24_wf_init(&ctx->wf, ctx->wf_backing, NRF24_FUSION_CH, WF_ROWS);

    /* display takeover */
    gui_direct_draw_acquire(app->gui);
    dd_acquired = true;
    ctx->panel = furi_hal_display_get_panel_handle();
    ctx->h_res = furi_hal_display_get_h_res();
    ctx->v_res = furi_hal_display_get_v_res();
    if(!ctx->panel || ctx->h_res <= 0 || ctx->v_res <= 0) {
        FURI_LOG_E(TAG, "no panel/dims (%d x %d)", ctx->h_res, ctx->v_res);
        goto fail;
    }

    ctx->stripe =
        heap_caps_malloc((size_t)ctx->h_res * STRIPE_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    ctx->xch = malloc((size_t)ctx->h_res);
    if(!ctx->stripe || !ctx->xch) goto fail;
    for(int x = 0; x < ctx->h_res; x++) {
        int ch = x * NRF24_FUSION_CH / ctx->h_res;
        if(ch >= NRF24_FUSION_CH) ch = NRF24_FUSION_CH - 1;
        ctx->xch[x] = (uint8_t)ch;
    }

    nrf24_hw_init();
    g_fusion_ctx = ctx;

    /* subscribe to raw input (direct-draw suppresses viewport input) */
    ctx->input = furi_record_open(RECORD_INPUT_EVENTS);
    ctx->input_sub = furi_pubsub_subscribe(ctx->input, fusion_input_callback, ctx);

    ctx->stop = false;
    ctx->sweep_worker = furi_thread_alloc_ex("Nrf24FusSweep", 4096, fusion_sweep_worker, ctx);
    ctx->render_worker = furi_thread_alloc_ex("Nrf24FusRender", 8192, fusion_render_worker, ctx);
    furi_thread_start(ctx->sweep_worker);
    furi_thread_start(ctx->render_worker);
    return;

fail:
    if(dd_acquired) gui_direct_draw_release(app->gui);
    if(ctx->stripe) heap_caps_free(ctx->stripe);
    if(ctx->xch) free(ctx->xch);
    if(ctx->wf_backing) free(ctx->wf_backing);
    if(ctx->lock) furi_mutex_free(ctx->lock);
    free(ctx);
    g_fusion_ctx = NULL;
    scene_manager_previous_scene(app->scene_manager);
}

bool nrf24_app_scene_fusion_spectrum_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    FusionCtx* ctx = g_fusion_ctx;
    if(event.type != SceneManagerEventTypeCustom || !ctx) return false;

    switch(event.event) {
    case FusionEventCursorLeft:
        ctx->cursor_on = true;
        if(ctx->cursor_ch) ctx->cursor_ch--;
        return true;
    case FusionEventCursorRight:
        ctx->cursor_on = true;
        if(ctx->cursor_ch < NRF24_FUSION_CH - 1) ctx->cursor_ch++;
        return true;
    case FusionEventExit:
        scene_manager_previous_scene(app->scene_manager);
        return true;
    default:
        return false;
    }
}

void nrf24_app_scene_fusion_spectrum_on_exit(void* context) {
    UNUSED(context);
    FusionCtx* ctx = g_fusion_ctx;
    if(!ctx) return;
    g_fusion_ctx = NULL;

    /* stop workers first so nothing else touches the radio or the panel */
    ctx->stop = true;
    if(ctx->render_worker) {
        furi_thread_join(ctx->render_worker);
        furi_thread_free(ctx->render_worker);
    }
    if(ctx->sweep_worker) {
        furi_thread_join(ctx->sweep_worker);
        furi_thread_free(ctx->sweep_worker);
    }

    if(ctx->input_sub) furi_pubsub_unsubscribe(ctx->input, ctx->input_sub);
    if(ctx->input) furi_record_close(RECORD_INPUT_EVENTS);

    /* power down the radio (no sweep can race now) */
    nrf24_hw_acquire();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    /* release the display (no render can race now) */
    gui_direct_draw_release(ctx->app->gui);

    if(ctx->stripe) heap_caps_free(ctx->stripe);
    if(ctx->xch) free(ctx->xch);
    if(ctx->wf_backing) free(ctx->wf_backing);
    if(ctx->lock) furi_mutex_free(ctx->lock);
    free(ctx);
}

#endif /* !PUBLIC_BUILD -- color takeover, private only */
