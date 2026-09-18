#include "subghz_fob_analyzer.h"

#include <furi.h>
#include <input/input.h>
#include <gui/elements.h>
#include <float_tools.h>

#include "../helpers/subghz_fob_analyzer_worker.h"

#define TAG "fob_analyzer"

struct SubGhzFobAnalyzer {
    View* view;
    SubGhzFobAnalyzerWorker* worker;
    SubGhzFobAnalyzerCallback callback;
    void* context;
    SubGhzTxRx* txrx;
};

typedef struct {
    uint32_t frequency;
    float rssi;
    bool signal;
    FobMod mod;
    FobEnc enc;
    uint32_t te_us;
    uint32_t bitrate;
    uint32_t fsk_dev_khz;
} SubGhzFobAnalyzerModel;

void subghz_fob_analyzer_set_callback(
    SubGhzFobAnalyzer* instance,
    SubGhzFobAnalyzerCallback callback,
    void* context) {
    furi_assert(instance);
    furi_assert(callback);
    instance->callback = callback;
    instance->context = context;
}

static const char* subghz_fob_analyzer_mod_name(FobMod mod) {
    switch(mod) {
    case FobModOok:
        return "OOK/ASK";
    case FobModFsk:
        return "2-FSK";
    case FobModFskNarrow:
        return "2-FSK nar";
    case FobModFskWide:
        return "2-FSK wide";
    default:
        return "--";
    }
}

static const char* subghz_fob_analyzer_enc_name(FobEnc enc) {
    switch(enc) {
    case FobEncPwm:
        return "PWM?";
    case FobEncPpm:
        return "PPM?";
    case FobEncManchester:
        return "Manch?";
    default:
        return "";
    }
}

// Recommended Flipper capture preset for the classified signal.
static const char* subghz_fob_analyzer_preset_name(FobMod mod, uint32_t te_us) {
    switch(mod) {
    case FobModOok:
        return (te_us > 0 && te_us < 150) ? "AM270" : "AM650";
    case FobModFskNarrow:
        return "FM238";
    case FobModFsk:
    case FobModFskWide:
        return "FM476";
    default:
        return "--";
    }
}

// Snap a wobbly detected carrier to the nearest standard fob frequency (within
// tolerance) for the Capture recommendation. A wide-FSK signal has two tones
// ~2*dev apart, so the peak-finder bounces ~+/-100kHz around the true centre;
// this prints a clean, actionable frequency to capture on.
static uint32_t subghz_fob_analyzer_snap_freq(uint32_t f) {
    static const uint32_t std[] = {
        300000000u, 303875000u, 304250000u, 310000000u, 315000000u, 318000000u,
        390000000u, 418000000u, 433075000u, 433420000u, 433920000u, 434420000u,
        434775000u, 438900000u, 868350000u, 868950000u, 915000000u,
    };
    uint32_t best = f;
    uint32_t best_d = 300000u; // +/-0.3 MHz snap window
    for(size_t i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
        uint32_t d = (f > std[i]) ? (f - std[i]) : (std[i] - f);
        if(d < best_d) {
            best_d = d;
            best = std[i];
        }
    }
    return best;
}

static void subghz_fob_analyzer_draw(Canvas* canvas, SubGhzFobAnalyzerModel* model) {
    char buffer[48] = {0};

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "Fob Analyzer");

    // Frequency (big)
    canvas_set_font(canvas, FontBigNumbers);
    if(model->signal && model->frequency > 0) {
        snprintf(
            buffer,
            sizeof(buffer),
            "%03ld.%03ld",
            model->frequency / 1000000 % 1000,
            model->frequency / 1000 % 1000);
    } else {
        snprintf(buffer, sizeof(buffer), "---.---");
    }
    canvas_draw_str_aligned(canvas, 60, 15, AlignCenter, AlignTop, buffer);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 108, 24, "MHz");

    // Modulation + RSSI
    canvas_set_font(canvas, FontSecondary);
    snprintf(buffer, sizeof(buffer), "Mod: %s", subghz_fob_analyzer_mod_name(model->mod));
    canvas_draw_str(canvas, 4, 42, buffer);
    if(model->signal && !float_is_equal(model->rssi, 0.f)) {
        snprintf(buffer, sizeof(buffer), "RSSI %d", (int)model->rssi);
        canvas_draw_str_aligned(canvas, 124, 42, AlignRight, AlignBottom, buffer);
    }

    // TE / bitrate / encoding
    if(model->signal) {
        bool is_fsk = (model->mod == FobModFsk || model->mod == FobModFskNarrow ||
                       model->mod == FobModFskWide);
        if(model->te_us > 0) {
            snprintf(
                buffer,
                sizeof(buffer),
                "TE ~%luus %lu.%lukb %s",
                model->te_us,
                model->bitrate / 1000,
                (model->bitrate % 1000) / 100,
                subghz_fob_analyzer_enc_name(model->enc));
        } else if(is_fsk) {
            if(model->fsk_dev_khz > 0) {
                snprintf(buffer, sizeof(buffer), "dev ~%lukHz  capture RAW", model->fsk_dev_khz);
            } else {
                snprintf(buffer, sizeof(buffer), "TE: capture RAW to read");
            }
        } else {
            snprintf(buffer, sizeof(buffer), "TE: --");
        }
        canvas_draw_str(canvas, 4, 52, buffer);
    } else {
        canvas_draw_str(canvas, 4, 52, "Press & HOLD the fob button");
    }

    // Verdict bar (inverted)
    if(model->signal) {
        canvas_draw_box(canvas, 0, 54, 128, 10);
        canvas_set_color(canvas, ColorWhite);
        uint32_t snap = subghz_fob_analyzer_snap_freq(model->frequency);
        snprintf(
            buffer,
            sizeof(buffer),
            "Capture: %03ld.%02ld %s",
            snap / 1000000 % 1000,
            snap / 10000 % 100,
            subghz_fob_analyzer_preset_name(model->mod, model->te_us));
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, buffer);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "scanning bands...");
    }
}

static bool subghz_fob_analyzer_input(InputEvent* event, void* context) {
    furi_assert(context);
    if(event->key == InputKeyBack) {
        return false; // let the scene manager pop
    }
    return true;
}

static void subghz_fob_analyzer_worker_callback(void* context, const FobAnalyzerResult* result) {
    SubGhzFobAnalyzer* instance = context;
    with_view_model(
        instance->view,
        SubGhzFobAnalyzerModel * model,
        {
            model->frequency = result->frequency;
            model->rssi = result->rssi;
            model->signal = result->signal;
            model->mod = result->mod;
            model->enc = result->enc;
            model->te_us = result->te_us;
            model->bitrate = result->bitrate;
            model->fsk_dev_khz = result->fsk_dev_khz;
        },
        true);
    if(result->signal && instance->callback) {
        instance->callback(SubGhzCustomEventSceneAnalyzerLock, instance->context);
    }
}

static void subghz_fob_analyzer_enter(void* context) {
    furi_assert(context);
    SubGhzFobAnalyzer* instance = context;

    instance->worker = subghz_fob_analyzer_worker_alloc(instance->context);
    subghz_fob_analyzer_worker_set_callback(
        instance->worker, subghz_fob_analyzer_worker_callback, instance);
    subghz_fob_analyzer_worker_start(instance->worker);

    with_view_model(
        instance->view,
        SubGhzFobAnalyzerModel * model,
        {
            model->frequency = 0;
            model->rssi = 0;
            model->signal = false;
            model->mod = FobModUnknown;
            model->enc = FobEncUnknown;
            model->te_us = 0;
            model->bitrate = 0;
            model->fsk_dev_khz = 0;
        },
        true);
}

static void subghz_fob_analyzer_exit(void* context) {
    furi_assert(context);
    SubGhzFobAnalyzer* instance = context;
    if(subghz_fob_analyzer_worker_is_running(instance->worker)) {
        subghz_fob_analyzer_worker_stop(instance->worker);
    }
    subghz_fob_analyzer_worker_free(instance->worker);
}

SubGhzFobAnalyzer* subghz_fob_analyzer_alloc(SubGhzTxRx* txrx) {
    SubGhzFobAnalyzer* instance = malloc(sizeof(SubGhzFobAnalyzer));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzFobAnalyzerModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, (ViewDrawCallback)subghz_fob_analyzer_draw);
    view_set_input_callback(instance->view, subghz_fob_analyzer_input);
    view_set_enter_callback(instance->view, subghz_fob_analyzer_enter);
    view_set_exit_callback(instance->view, subghz_fob_analyzer_exit);
    instance->txrx = txrx;
    instance->callback = NULL;
    instance->context = NULL;

    return instance;
}

void subghz_fob_analyzer_free(SubGhzFobAnalyzer* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* subghz_fob_analyzer_get_view(SubGhzFobAnalyzer* instance) {
    furi_assert(instance);
    return instance->view;
}
