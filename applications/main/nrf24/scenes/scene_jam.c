/* nRF24 jammer scene — the single consolidated jammer, now living in the nRF24
 * app menu alongside Spectrum + MouseJacker. Bruce v1.14 mode-select base +
 * this session's improvements: concentrated channel maps, research-correct
 * strategy per protocol (OFDM WiFi / DSSS Zigbee -> Turbo noise-flood; GFSK ->
 * zero-dwell swept CW tone), BT Classic + Zigbee, full PA MAX (unclamped — the
 * Hydra/RF-Pack boards have the caps). Scrolling-list nav (Down/OK) via
 * nrf24_jam_view. */

#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_export.h"

#include <furi.h>
#include <stdlib.h>
#include <esp_rom_sys.h>

#define TAG      "Nrf24Jam"
#define PA_MAX   3 /* Nrf24Pa_Max */
#define RATE_2M  1 /* Nrf24Rate_2M */
#define BATCH_MS 30

/* ---- Concentrated channel maps (channel = MHz - 2400), indexed by mode ---- */
static const uint8_t CH_WIFI[] = {1,  4,  7,  10, 13, 16, 19, 22, 26, 29, 32, 35,
                                  38, 41, 44, 47, 51, 54, 57, 60, 63, 66, 69, 72};
static const uint8_t CH_BT[] =
    {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
static const uint8_t CH_BLE_ADV[] = {2, 26, 80};
static const uint8_t CH_ZIGBEE[] = {5,  10, 15, 20, 25, 30, 35, 40,
                                    45, 50, 55, 60, 65, 70, 75, 80};
static const uint8_t CH_RC[] = {1, 3, 5, 7};
static const uint8_t CH_USB[] = {40, 50, 60};
static const uint8_t CH_VIDEO[] = {70, 75, 80};
static uint8_t buf_bledata[40]; /* 2,4,...,80 (built at worker start) */

typedef struct {
    const uint8_t* list; /* NULL => full-band 0..83 sweep */
    size_t count;
} ModeCh;

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    volatile bool desired_running;
    volatile int mode;
    volatile uint8_t pa_level; /* nRF24 RF_PWR 0..3 (Min/Low/High/Max), live-adjustable */
    uint16_t saved_dwell; /* dwell as loaded from shared settings; passed back unchanged on save */
} JamCtx;

static JamCtx* g_ctx = NULL;

/* One reactive pass over the 3 BLE advertising channels: listen on RPD, and the
 * instant a carrier appears fire a short CW pulse, then retune to the next adv
 * channel. Runs inside the acquired bus. */
#ifndef PUBLIC_BUILD
static void jam_reactive_pass(JamCtx* ctx) {
    static const uint8_t BLE_ADV[3] = {2, 26, 80};
    for(int k = 0; k < 3 && !ctx->stop && ctx->desired_running &&
                   nrf24_jam_mode_is_reactive(ctx->mode);
        k++) {
        uint8_t ch = BLE_ADV[k];
        nrf24_hw_reactive_setup(ch);
        nrf24_hw_ce(true);
        esp_rom_delay_us(130); /* Tstby2a -> RX */
        for(int t = 0; t < 40 && !ctx->stop && ctx->desired_running; t++) { /* ~2ms window */
            if(nrf24_hw_read_rpd_raw()) {
                nrf24_hw_ce(false);
                nrf24_hw_react_to_tx();
                nrf24_hw_ce(true);
                esp_rom_delay_us(430); /* 130 settle + ~300 CW pulse */
                nrf24_hw_ce(false);
                nrf24_hw_react_to_rx(ch);
                break;
            }
            esp_rom_delay_us(50);
        }
        nrf24_hw_ce(false);
    }
}
#endif

static int32_t jam_worker(void* context) {
    JamCtx* ctx = context;
    Nrf24App* app = ctx->app;

    for(size_t k = 0; k < sizeof(buf_bledata); k++) buf_bledata[k] = (uint8_t)(2 + 2 * k);

    const ModeCh chans[NRF24_JAM_MODE_COUNT] = {
        {CH_WIFI, sizeof(CH_WIFI)},       /* 0 WiFi (flood) */
        {CH_BT, sizeof(CH_BT)},           /* 1 BT Classic  */
        {CH_BLE_ADV, sizeof(CH_BLE_ADV)}, /* 2 BLE Adv     */
        {buf_bledata, sizeof(buf_bledata)}, /* 3 BLE Data  */
        {CH_ZIGBEE, sizeof(CH_ZIGBEE)},   /* 4 Zigbee (flood) */
        {CH_RC, sizeof(CH_RC)},           /* 5 RC          */
        {CH_USB, sizeof(CH_USB)},         /* 6 USB         */
        {CH_VIDEO, sizeof(CH_VIDEO)},     /* 7 Video       */
        {NULL, 0},                        /* 8 Full Band   */
#ifndef PUBLIC_BUILD
        {CH_BLE_ADV, sizeof(CH_BLE_ADV)}, /* 9 BLE Reactive (jam_reactive_pass drives its own hop) */
#endif
    };

    nrf24_hw_init();
    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();
    with_view_model(app->jam_view, Nrf24JamModel * m, { m->hardware_ok = ok; }, true);
    if(!ok) {
        FURI_LOG_W(TAG, "nRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    nrf24_hw_pa_unclamped(true); /* full PA MAX on USB (boards have the caps) */

    uint32_t hz = furi_kernel_get_tick_frequency();
    bool active = false, cur_flood = false, cur_reactive = false;
    int cur = -1;
    uint8_t cur_pa = 0xFF; /* forces a (re)start when pa_level is first applied or changed live */
    size_t i = 0;

    while(!ctx->stop) {
        bool want = ctx->desired_running;
        int mi = ctx->mode;
        const ModeCh* m = &chans[mi];
        bool flood = nrf24_jam_mode_is_flood(mi);
        bool reactive = nrf24_jam_mode_is_reactive(mi);

        if(want && (!active || mi != cur || ctx->pa_level != cur_pa)) {
            nrf24_hw_acquire();
            if(active) {
                if(cur_reactive) {
                    nrf24_hw_ce(false);
                    nrf24_hw_power_down();
                } else if(cur_flood) {
                    nrf24_hw_flood_stop();
                } else {
                    nrf24_hw_jammer_stop();
                }
            }
            if(reactive) {
                /* jam_reactive_pass() fully configures the chip (reactive_setup)
                 * on its first pass -- skip the jammer/flood start here so we
                 * don't key a stray carrier before the reactive loop takes over. */
            } else {
                uint8_t ch0 = m->count ? m->list[0] : 0;
                uint8_t pa = ctx->pa_level; /* live-selected PA level */
                if(flood) nrf24_hw_flood_start_ex(ch0, RATE_2M, pa);
                else nrf24_hw_jammer_start_ex(ch0, pa);
                /* --- TEMP DIAGNOSTIC (weak-RF debug) — remove after root cause.
                 * Dumps what the TX path actually programmed. Expected for a
                 * genuine nRF24L01+ CW jammer: Pvar=1 RF_SETUP=0x9F (CONT_WAVE|
                 * PLL|2M|PA_MAX|LNA) CONFIG has PWR_UP(0x02). A wrong Pvar or an
                 * RF_SETUP missing CONT_WAVE(0x80)/PA_MAX(0x06) => the bug. */
                FURI_LOG_I(
                    TAG,
                    "JAMDIAG mode=%d flood=%d Pvar=%d rate=%u RF_SETUP=0x%02X CONFIG=0x%02X ch0=%u",
                    mi,
                    (int)flood,
                    (int)nrf24_hw_is_p_variant(),
                    nrf24_hw_last_data_rate(),
                    nrf24_hw_read_rf_setup(),
                    nrf24_hw_read_config(),
                    (unsigned)ch0);
            }
            nrf24_hw_release();
            active = true;
            cur = mi;
            cur_flood = flood;
            cur_reactive = reactive;
            cur_pa = ctx->pa_level;
            i = 0;
        } else if(!want && active) {
            nrf24_hw_acquire();
            if(cur_reactive) {
                nrf24_hw_ce(false);
                nrf24_hw_power_down();
            } else if(cur_flood) {
                nrf24_hw_flood_stop();
            } else {
                nrf24_hw_jammer_stop();
            }
            nrf24_hw_release();
            active = false;
            cur = -1;
        }

        if(active) {
            nrf24_hw_acquire();
#ifndef PUBLIC_BUILD
            if(reactive) {
                jam_reactive_pass(ctx);
            } else
#endif
            {
                uint32_t end = furi_get_tick() + (BATCH_MS * hz) / 1000;
                while(!ctx->stop && ctx->desired_running && ctx->mode == mi &&
                      furi_get_tick() < end) {
                    uint8_t ch = m->count ? m->list[i % m->count] : (uint8_t)(i % 84);
                    if(flood) nrf24_hw_flood_burst(ch, 12, true);
                    else nrf24_hw_jammer_set_channel(ch);
                    i++;
                }
            }
            nrf24_hw_release();
            furi_delay_ms(1); /* yield to GUI/input (worker below UI priority) */
        } else {
            furi_delay_ms(20);
        }
    }

    if(active) {
        nrf24_hw_acquire();
        if(cur_reactive) {
            nrf24_hw_ce(false);
            nrf24_hw_power_down();
        } else if(cur_flood) {
            nrf24_hw_flood_stop();
        } else {
            nrf24_hw_jammer_stop();
        }
        nrf24_hw_release();
    }
    nrf24_hw_pa_unclamped(false);
    nrf24_hw_deinit();
    return 0;
}

void nrf24_app_scene_jam_on_enter(void* context) {
    Nrf24App* app = context;

    JamCtx* ctx = malloc(sizeof(JamCtx));
    if(!ctx) return;
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = false;
    uint8_t saved_mode = 0, saved_pa = PA_MAX;
    uint16_t saved_dwell = 250;
    nrf24_settings_load(&saved_mode, &saved_dwell, &saved_pa);
    /* Clamp the persisted mode to the count compiled into THIS build: the
     * validator (nrf24_export.c) can't know the per-build ceiling, and a config
     * carried from a PRIVATE build (or a hand-edited rflab.conf) can hold a mode
     * that indexes past the smaller PUBLIC chans[] array in the jam worker. */
    if(saved_mode >= NRF24_JAM_MODE_COUNT) saved_mode = 0;
    ctx->mode = saved_mode;
    ctx->saved_dwell = saved_dwell;
    ctx->pa_level = saved_pa;
    g_ctx = ctx;

    with_view_model(
        app->jam_view,
        Nrf24JamModel * model,
        {
            model->mode = saved_mode;
            model->running = false;
            model->hardware_ok = true;
            model->pa_level = saved_pa;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewJam);

    ctx->worker = furi_thread_alloc_ex("Nrf24Jam", 4096, jam_worker, ctx);
    /* Below Normal so a keypress preempts the CPU-bound sweep loop (no UI freeze). */
    furi_thread_set_priority(ctx->worker, FuriThreadPriorityLow);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_jam_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    switch(event.event) {
    case Nrf24JamEventToggle: {
        bool nr = !g_ctx->desired_running;
        g_ctx->desired_running = nr;
        with_view_model(app->jam_view, Nrf24JamModel * m, { m->running = nr; }, true);
        return true;
    }
    case Nrf24JamEventNext: {
        int nm = (g_ctx->mode + 1) % NRF24_JAM_MODE_COUNT;
        g_ctx->mode = nm;
        with_view_model(app->jam_view, Nrf24JamModel * m, { m->mode = nm; }, true);
        return true;
    }
    case Nrf24JamEventPrev: {
        int nm = (g_ctx->mode + NRF24_JAM_MODE_COUNT - 1) % NRF24_JAM_MODE_COUNT;
        g_ctx->mode = nm;
        with_view_model(app->jam_view, Nrf24JamModel * m, { m->mode = nm; }, true);
        return true;
    }
    case Nrf24JamEventPaDown: {
        uint8_t pa = g_ctx->pa_level ? (uint8_t)(g_ctx->pa_level - 1) : 0;
        g_ctx->pa_level = pa; /* worker re-keys the carrier at the new level (live) */
        with_view_model(app->jam_view, Nrf24JamModel * m, { m->pa_level = pa; }, true);
        return true;
    }
    case Nrf24JamEventPaUp: {
        uint8_t pa = (g_ctx->pa_level < 3) ? (uint8_t)(g_ctx->pa_level + 1) : 3;
        g_ctx->pa_level = pa; /* worker re-keys the carrier at the new level (live) */
        with_view_model(app->jam_view, Nrf24JamModel * m, { m->pa_level = pa; }, true);
        return true;
    }
    default:
        return false;
    }
}

void nrf24_app_scene_jam_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    nrf24_settings_save((uint8_t)g_ctx->mode, g_ctx->saved_dwell, g_ctx->pa_level);
    free(g_ctx);
    g_ctx = NULL;
}
