#include "subghz_view_protoview.h"
#include "../helpers/protoview_ring.h"
#include <gui/elements.h>
#include <input/input.h>
#include <furi.h>
#include <string.h>

// ProtoView view: a 128x64 OOK waveform scope + a bit editor.
//
// Scope mode renders one column per ring sample (high = upper trace, low = lower
// trace) and scrolls with Left/Right. Edit mode renders the decoded bitstring and
// lets the user flip individual bits before replay. All heavy state (the sample
// ring) lives in the pure protoview_ring core; this view only reads its snapshot.

#define PV_WAVE_TOP 16
#define PV_WAVE_BOT 44
#define PV_CHAR_W   6

struct SubGhzViewProtoview {
    View* view;
    SubGhzViewProtoviewCallback callback;
    void* context;
    char bits_out[257];
};

typedef struct {
    ProtoviewMode mode;
    size_t scroll;
    size_t cursor;
    bool frozen;
    char bits[257];
    char label[24];
    char status[24];
} SubGhzViewProtoviewModel;

void subghz_view_protoview_set_callback(
    SubGhzViewProtoview* v,
    SubGhzViewProtoviewCallback cb,
    void* ctx) {
    furi_assert(v);
    v->callback = cb;
    v->context = ctx;
}

static void subghz_view_protoview_fire(SubGhzViewProtoview* v, ProtoviewEvent evt) {
    if(v->callback) v->callback((uint32_t)evt, v->context);
}

void subghz_view_protoview_set_frozen(SubGhzViewProtoview* v, bool frozen) {
    furi_assert(v);
    with_view_model(
        v->view, SubGhzViewProtoviewModel * model, { model->frozen = frozen; }, true);
}

void subghz_view_protoview_set_bits(SubGhzViewProtoview* v, const char* bits, const char* label) {
    furi_assert(v);
    with_view_model(
        v->view,
        SubGhzViewProtoviewModel * model,
        {
            strncpy(model->bits, bits ? bits : "", sizeof(model->bits) - 1);
            model->bits[sizeof(model->bits) - 1] = '\0';
            strncpy(model->label, label ? label : "", sizeof(model->label) - 1);
            model->label[sizeof(model->label) - 1] = '\0';
            model->cursor = 0;
        },
        true);
}

void subghz_view_protoview_set_status(SubGhzViewProtoview* v, const char* freq_preset) {
    furi_assert(v);
    with_view_model(
        v->view,
        SubGhzViewProtoviewModel * model,
        {
            strncpy(model->status, freq_preset ? freq_preset : "", sizeof(model->status) - 1);
            model->status[sizeof(model->status) - 1] = '\0';
        },
        true);
}

const char* subghz_view_protoview_get_bits(SubGhzViewProtoview* v) {
    furi_assert(v);
    with_view_model(
        v->view,
        SubGhzViewProtoviewModel * model,
        {
            strncpy(v->bits_out, model->bits, sizeof(v->bits_out) - 1);
            v->bits_out[sizeof(v->bits_out) - 1] = '\0';
        },
        false);
    return v->bits_out;
}

static void subghz_view_protoview_draw_scope(Canvas* canvas, SubGhzViewProtoviewModel* model) {
    const PvSample* w = NULL;
    size_t n = pv_ring_snapshot(&w);

    canvas_set_font(canvas, FontSecondary);

    if(n == 0 || !w) {
        canvas_draw_str(canvas, 2, 30, "Waiting for signal...");
        canvas_draw_str(canvas, 0, 63, "OK:Frz \x7f:Dec Up:Edit");
        return;
    }

    /* Right-key scroll is unbounded in the input handler; clamp here so scrolling
     * past the end can't leave the scope permanently blank. */
    if(model->scroll >= n) model->scroll = n - 1;

    // Time-proportional trace: each sample is a horizontal run whose WIDTH scales
    // with its duration, with a vertical edge at every level change — i.e. a real
    // logic-analyzer view where short pulses, long pulses and sync gaps are
    // visually distinct. (The old renderer drew one pixel per edge and ignored
    // duration entirely, so every signal looked like a meaningless picket fence.)
    //
    // Auto-scale: the shortest pulse in the on-screen window sets the unit (~3 px),
    // so the trace fills the width whatever the protocol's timing.
    uint32_t unit = 0xFFFFFFFFu;
    size_t scan_end = model->scroll + 256;
    if(scan_end > n) scan_end = n;
    for(size_t i = model->scroll; i < scan_end; i++) {
        if(w[i].dur < unit) unit = w[i].dur;
    }
    if(unit < 1) unit = 1;

    int x = 0;
    int prev_level = -1;
    for(size_t i = model->scroll; i < n && x < 128; i++) {
        int level = w[i].level ? 1 : 0;
        int y = level ? PV_WAVE_TOP : PV_WAVE_BOT;

        int run = (int)(((uint64_t)w[i].dur * 3u) / unit);
        if(run < 1) run = 1;

        // vertical edge at a level transition
        if(prev_level >= 0 && prev_level != level) {
            canvas_draw_line(canvas, x, PV_WAVE_TOP, x, PV_WAVE_BOT);
        }
        int x2 = x + run;
        if(x2 > 128) x2 = 128;
        canvas_draw_line(canvas, x, y, x2 - 1, y);
        x = x2;
        prev_level = level;
    }

    // cursor marker at left edge of window
    canvas_draw_line(canvas, 0, PV_WAVE_TOP - 3, 0, PV_WAVE_BOT + 3);

    char foot[42];
    snprintf(
        foot,
        sizeof(foot),
        "%s %u/%u",
        model->label[0] ? model->label : "OK:Frz \x7f:Dec Up:Edit",
        (unsigned)model->scroll,
        (unsigned)n);
    canvas_draw_str(canvas, 0, 63, foot);
}

static void subghz_view_protoview_draw_edit(Canvas* canvas, SubGhzViewProtoviewModel* model) {
    size_t len = strlen(model->bits);
    const size_t window = 20;
    size_t start = 0;
    if(model->cursor >= window) start = model->cursor - window + 1;

    canvas_set_font(canvas, FontSecondary);
    for(size_t k = 0; k < window; k++) {
        size_t idx = start + k;
        if(idx >= len) break;
        int x = (int)(k * PV_CHAR_W);
        char cbuf[2] = {model->bits[idx], '\0'};
        if(idx == model->cursor) {
            canvas_draw_box(canvas, x, 22, PV_CHAR_W, 11);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, x, 31, cbuf);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, x, 31, cbuf);
        }
    }

    char foot[42];
    snprintf(foot, sizeof(foot), "%s bit %u/%u", model->label, (unsigned)model->cursor, (unsigned)len);
    canvas_draw_str(canvas, 0, 45, foot);
    canvas_draw_str(canvas, 0, 63, "Up:flip L/R:move OK:TX OK\x7f:Save");
}

static void subghz_view_protoview_draw(Canvas* canvas, SubGhzViewProtoviewModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 7, model->status);
    if(model->frozen) canvas_draw_str(canvas, 110, 7, "[F]");
    canvas_draw_line(canvas, 0, 9, 128, 9);

    if(model->mode == ProtoviewModeScope) {
        subghz_view_protoview_draw_scope(canvas, model);
    } else {
        subghz_view_protoview_draw_edit(canvas, model);
    }
}

static bool subghz_view_protoview_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewProtoview* v = context;

    if(event->type != InputTypeShort && event->type != InputTypeLong &&
       event->type != InputTypeRepeat) {
        return false;
    }

    ProtoviewMode mode = ProtoviewModeScope;
    with_view_model(
        v->view, SubGhzViewProtoviewModel * model, { mode = model->mode; }, false);

    if(mode == ProtoviewModeScope) {
        switch(event->key) {
        case InputKeyLeft:
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                {
                    if(model->scroll >= 8)
                        model->scroll -= 8;
                    else
                        model->scroll = 0;
                },
                true);
            return true;
        case InputKeyRight:
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                { model->scroll += 8; },
                true);
            return true;
        case InputKeyOk:
            if(event->type == InputTypeLong) {
                subghz_view_protoview_fire(v, ProtoviewEvtDecode);
            } else {
                subghz_view_protoview_fire(v, ProtoviewEvtFreeze);
            }
            return true;
        case InputKeyUp:
            // enter edit mode (only meaningful once bits exist)
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                {
                    if(strlen(model->bits) > 0) model->mode = ProtoviewModeEdit;
                },
                true);
            subghz_view_protoview_fire(v, ProtoviewEvtEdit);
            return true;
        case InputKeyDown:
            subghz_view_protoview_fire(v, ProtoviewEvtSave);
            return true;
        case InputKeyBack:
            subghz_view_protoview_fire(v, ProtoviewEvtBack);
            return true;
        default:
            return false;
        }
    } else { // Edit mode
        switch(event->key) {
        case InputKeyLeft:
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                {
                    if(model->cursor > 0) model->cursor--;
                },
                true);
            return true;
        case InputKeyRight:
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                {
                    size_t len = strlen(model->bits);
                    if(len > 0 && model->cursor + 1 < len) model->cursor++;
                },
                true);
            return true;
        case InputKeyUp:
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                {
                    if(model->cursor < strlen(model->bits)) {
                        model->bits[model->cursor] =
                            (model->bits[model->cursor] == '0') ? '1' : '0';
                    }
                },
                true);
            return true;
        case InputKeyOk:
            if(event->type == InputTypeLong) {
                subghz_view_protoview_fire(v, ProtoviewEvtSave);
            } else {
                subghz_view_protoview_fire(v, ProtoviewEvtReplay);
            }
            return true;
        case InputKeyBack:
            // return to scope; do not exit the scene
            with_view_model(
                v->view,
                SubGhzViewProtoviewModel * model,
                { model->mode = ProtoviewModeScope; },
                true);
            return true;
        default:
            return false;
        }
    }
}

SubGhzViewProtoview* subghz_view_protoview_alloc(void) {
    SubGhzViewProtoview* v = malloc(sizeof(SubGhzViewProtoview));
    v->callback = NULL;
    v->context = NULL;
    v->bits_out[0] = '\0';
    v->view = view_alloc();
    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(SubGhzViewProtoviewModel));
    view_set_context(v->view, v);
    view_set_draw_callback(v->view, (ViewDrawCallback)subghz_view_protoview_draw);
    view_set_input_callback(v->view, subghz_view_protoview_input);

    with_view_model(
        v->view,
        SubGhzViewProtoviewModel * model,
        {
            model->mode = ProtoviewModeScope;
            model->scroll = 0;
            model->cursor = 0;
            model->frozen = false;
            model->bits[0] = '\0';
            model->label[0] = '\0';
            model->status[0] = '\0';
        },
        true);

    return v;
}

void subghz_view_protoview_free(SubGhzViewProtoview* v) {
    furi_assert(v);
    view_free(v->view);
    free(v);
}

View* subghz_view_protoview_get_view(SubGhzViewProtoview* v) {
    furi_assert(v);
    return v->view;
}
