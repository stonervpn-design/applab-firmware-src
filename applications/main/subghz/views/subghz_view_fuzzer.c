#include "subghz_view_fuzzer.h"
#include <gui/elements.h>
#include <input/input.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

// Fuzzer run view. Layout (128x64, 1-bit):
//   line1 y7  : title + frequency, TX blink box top-right
//   line2 y19 : mode + protocol label (caller-supplied)
//   line3 y29 : "Frame N / TOTAL"
//   line4 y39 : mutation descriptor (caller-supplied)
//   footer y63: control-key hint

#define SVF_STR_LEN 40

struct SubGhzViewFuzzer {
    View* view;
    SubGhzViewFuzzerCallback callback;
    void* context;
};

typedef struct {
    uint32_t freq;
    char mode_str[SVF_STR_LEN];
    char mutation_str[SVF_STR_LEN];
    uint32_t index;
    uint32_t total;
    bool tx_on;
    bool have_data;
} SubGhzViewFuzzerModel;

void subghz_view_fuzzer_set_callback(
    SubGhzViewFuzzer* instance,
    SubGhzViewFuzzerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

static void subghz_view_fuzzer_fire(SubGhzViewFuzzer* instance, SubGhzViewFuzzerEvent event) {
    if(instance->callback) instance->callback(event, instance->context);
}

void subghz_view_fuzzer_set_data(
    SubGhzViewFuzzer* instance,
    uint32_t freq,
    const char* mode_str,
    const char* mutation_str,
    uint32_t index,
    uint32_t total,
    bool tx_on) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        SubGhzViewFuzzerModel * model,
        {
            model->freq = freq;
            if(mode_str) {
                strncpy(model->mode_str, mode_str, SVF_STR_LEN - 1);
                model->mode_str[SVF_STR_LEN - 1] = '\0';
            } else {
                model->mode_str[0] = '\0';
            }
            if(mutation_str) {
                strncpy(model->mutation_str, mutation_str, SVF_STR_LEN - 1);
                model->mutation_str[SVF_STR_LEN - 1] = '\0';
            } else {
                model->mutation_str[0] = '\0';
            }
            model->index = index;
            model->total = total;
            model->tx_on = tx_on;
            model->have_data = true;
        },
        true);
}

static void subghz_view_fuzzer_draw(Canvas* canvas, SubGhzViewFuzzerModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    // Line 1: title + frequency.
    char freq_lbl[24];
    snprintf(
        freq_lbl,
        sizeof(freq_lbl),
        "Fuzzer %lu.%02lu MHz",
        (unsigned long)(model->freq / 1000000UL),
        (unsigned long)((model->freq % 1000000UL) / 10000UL));
    canvas_draw_str(canvas, 0, 7, freq_lbl);
    canvas_draw_line(canvas, 0, 9, 127, 9);

    // TX blink box, top-right: filled when actively transmitting, outline otherwise.
    if(model->tx_on) {
        canvas_draw_box(canvas, 118, 1, 8, 6);
    } else {
        canvas_draw_frame(canvas, 118, 1, 8, 6);
    }

    if(!model->have_data) {
        canvas_draw_str(canvas, 2, 30, "idle...");
        canvas_draw_str(canvas, 0, 63, "OK:Run L:Stop R:Step D:Mark");
        return;
    }

    // Line 2: mode + protocol.
    canvas_draw_str(canvas, 0, 19, model->mode_str);

    // Line 3: frame index / total.
    char frame_lbl[24];
    snprintf(
        frame_lbl,
        sizeof(frame_lbl),
        "Frame %lu / %lu",
        (unsigned long)model->index,
        (unsigned long)model->total);
    canvas_draw_str(canvas, 0, 29, frame_lbl);

    // Line 4: mutation descriptor.
    canvas_draw_str(canvas, 0, 39, model->mutation_str);

    canvas_draw_line(canvas, 0, 53, 127, 53);
    canvas_draw_str(canvas, 0, 63, "OK:Run L:Stop R:Step D:Mark");
}

static bool subghz_view_fuzzer_input(InputEvent* event, void* context) {
    furi_assert(context);
    SubGhzViewFuzzer* instance = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;
    switch(event->key) {
    case InputKeyOk:
        subghz_view_fuzzer_fire(instance, SubGhzViewFuzzerEventStartPause);
        return true;
    case InputKeyLeft:
        subghz_view_fuzzer_fire(instance, SubGhzViewFuzzerEventStop);
        return true;
    case InputKeyRight:
        subghz_view_fuzzer_fire(instance, SubGhzViewFuzzerEventStep);
        return true;
    case InputKeyDown:
        subghz_view_fuzzer_fire(instance, SubGhzViewFuzzerEventMark);
        return true;
    default:
        return false; // Back -> scene-manager navigation exits the scene
    }
}

SubGhzViewFuzzer* subghz_view_fuzzer_alloc(void) {
    SubGhzViewFuzzer* instance = malloc(sizeof(SubGhzViewFuzzer));
    instance->callback = NULL;
    instance->context = NULL;
    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzViewFuzzerModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, (ViewDrawCallback)subghz_view_fuzzer_draw);
    view_set_input_callback(instance->view, subghz_view_fuzzer_input);

    with_view_model(
        instance->view,
        SubGhzViewFuzzerModel * model,
        {
            memset(model, 0, sizeof(SubGhzViewFuzzerModel));
            model->have_data = false;
        },
        true);

    return instance;
}

void subghz_view_fuzzer_free(SubGhzViewFuzzer* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* subghz_view_fuzzer_get_view(SubGhzViewFuzzer* instance) {
    furi_assert(instance);
    return instance->view;
}
