#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_spectrum_calc.h"
#include "../helpers/nrf24_export.h"

#include <furi.h>

#define TAG "Nrf24Spectrum"

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    volatile uint16_t dwell_us;
    uint8_t saved_mode; /* jam mode as loaded from shared settings; passed back unchanged on save */
    uint8_t saved_pa; /* PA level as loaded from shared settings; passed back unchanged on save */
} Nrf24SpectrumCtx;

static Nrf24SpectrumCtx* g_ctx = NULL;

static int32_t nrf24_spectrum_worker(void* context) {
    Nrf24SpectrumCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = ok;
            model->running = ok;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->levels[i] = 0;
            model->sweep_count = 0;
            model->max_hold = false;
            model->cursor_ch = 0;
            model->cursor_on = false;
            model->dwell_us = 250;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->peaks[i] = 0;
        },
        true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    uint8_t local_levels[NRF24_SPECTRUM_CHANNELS] = {0};

    while(!ctx->stop) {
        /* Scope the nRF24 SPI acquire PER-CHANNEL, not across the whole sweep.
         * nrf24_hw_acquire() takes the global furi_hal_spi_bus_lock, which the
         * LCD refresh (furi_hal_display_commit) also needs; holding it across
         * all channels (~25ms) starves the display -> faint/garbled screen.
         * Releasing between channels lets the LCD commit interleave. */
        for(uint8_t ch = 0; ch < NRF24_SPECTRUM_CHANNELS && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            uint8_t rpd = nrf24_hw_listen_rpd_dwell(ch, ctx->dwell_us);
            nrf24_hw_release();
            local_levels[ch] = (uint8_t)((local_levels[ch] * 3 + rpd * 125) / 4);
        }

        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * model,
            {
                memcpy(model->levels, local_levels, NRF24_SPECTRUM_CHANNELS);
                model->sweep_count++;
                if(model->max_hold) {
                    for(int j = 0; j < NRF24_SPECTRUM_CHANNELS; j++)
                        model->peaks[j] = nrf24_maxhold_update(model->peaks[j], local_levels[j]);
                }
            },
            true);

        furi_delay_ms(20);
    }

    nrf24_hw_acquire();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();

    return 0;
}

void nrf24_app_scene_spectrum_on_enter(void* context) {
    Nrf24App* app = context;

    Nrf24SpectrumCtx* ctx = malloc(sizeof(Nrf24SpectrumCtx));
    if(!ctx) return;
    ctx->app = app;
    ctx->stop = false;
    uint8_t saved_mode = 0, saved_pa = 3;
    uint16_t saved_dwell = 250;
    nrf24_settings_load(&saved_mode, &saved_dwell, &saved_pa);
    ctx->dwell_us = saved_dwell;
    ctx->saved_mode = saved_mode;
    ctx->saved_pa = saved_pa;
    g_ctx = ctx;

    with_view_model(
        app->spectrum_view,
        Nrf24SpectrumModel * model,
        {
            model->hardware_ok = true;
            model->running = false;
            model->sweep_count = 0;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->levels[i] = 0;
            model->max_hold = false;
            model->cursor_ch = 0;
            model->cursor_on = false;
            model->dwell_us = saved_dwell;
            for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) model->peaks[i] = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSpectrum);

    ctx->worker = furi_thread_alloc_ex("Nrf24Spectrum", 4096, nrf24_spectrum_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;
    switch(event.event) {
    case Nrf24SpectrumEventCursorLeft:
        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * m,
            {
                m->cursor_on = true;
                if(m->cursor_ch) m->cursor_ch--;
            },
            true);
        return true;
    case Nrf24SpectrumEventCursorRight:
        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * m,
            {
                m->cursor_on = true;
                if(m->cursor_ch < NRF24_SPECTRUM_CHANNELS - 1) m->cursor_ch++;
            },
            true);
        return true;
    case Nrf24SpectrumEventToggleMaxHold:
        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * m,
            {
                m->max_hold = !m->max_hold;
                if(!m->max_hold)
                    for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) m->peaks[i] = 0;
            },
            true);
        return true;
    case Nrf24SpectrumEventDwellCycle: {
        static uint8_t di = 1; /* 250us */
        di = nrf24_dwell_next(di);
        g_ctx->dwell_us = nrf24_dwell_presets[di];
        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * m,
            { m->dwell_us = g_ctx->dwell_us; },
            true);
        return true;
    }
    case Nrf24SpectrumEventExport: {
        uint8_t snap[NRF24_SPECTRUM_CHANNELS];
        with_view_model(
            app->spectrum_view,
            Nrf24SpectrumModel * m,
            { memcpy(snap, m->levels, NRF24_SPECTRUM_CHANNELS); },
            false);
        nrf24_export_scan(snap, NRF24_SPECTRUM_CHANNELS);
        return true;
    }
    default:
        return false;
    }
}

void nrf24_app_scene_spectrum_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    nrf24_settings_save(g_ctx->saved_mode, g_ctx->dwell_us, g_ctx->saved_pa);
    free(g_ctx);
    g_ctx = NULL;
}
