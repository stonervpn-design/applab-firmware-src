#include "ble_fuzz_view.h"

#ifndef PUBLIC_BUILD

#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

// Layout (128x64, 1-bit):
//   line1 y10 : target name, TX blink box top-right
//   line2 y21 : address string
//   line3 y32 : mode label
//   line4 y43 : "N/TOTAL  cnt:counter"
//   footer y63: control-key hint

#define BFV_NAME_LEN 32
#define BFV_ADDR_LEN 20
#define BFV_MODE_LEN 32

struct BleFuzzView {
    View* view;
    BleFuzzViewCallback callback;
    void* context;
};

typedef struct {
    char target_name[BFV_NAME_LEN];
    char addr_str[BFV_ADDR_LEN];
    char mode_str[BFV_MODE_LEN];
    uint32_t index;
    uint32_t total;
    uint32_t counter;
    bool tx_on;
    bool have_data;
} BleFuzzViewModel;

static void ble_fuzz_view_fire(BleFuzzView* instance, BleFuzzViewEvent event) {
    if(instance->callback) instance->callback(event, instance->context);
}

void ble_fuzz_view_set_callback(
    BleFuzzView* instance,
    BleFuzzViewCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void ble_fuzz_view_set_data(
    BleFuzzView* instance,
    const char* target_name,
    const char* addr_str,
    const char* mode_str,
    uint32_t index,
    uint32_t total,
    uint32_t counter,
    bool tx_on) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        BleFuzzViewModel * model,
        {
            if(target_name) {
                strncpy(model->target_name, target_name, BFV_NAME_LEN - 1);
                model->target_name[BFV_NAME_LEN - 1] = '\0';
            } else {
                model->target_name[0] = '\0';
            }
            if(addr_str) {
                strncpy(model->addr_str, addr_str, BFV_ADDR_LEN - 1);
                model->addr_str[BFV_ADDR_LEN - 1] = '\0';
            } else {
                model->addr_str[0] = '\0';
            }
            if(mode_str) {
                strncpy(model->mode_str, mode_str, BFV_MODE_LEN - 1);
                model->mode_str[BFV_MODE_LEN - 1] = '\0';
            } else {
                model->mode_str[0] = '\0';
            }
            model->index = index;
            model->total = total;
            model->counter = counter;
            model->tx_on = tx_on;
            model->have_data = true;
        },
        true);
}

static void ble_fuzz_view_draw(Canvas* canvas, BleFuzzViewModel* model) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);

    // Line 1: target name.
    const char* name = model->target_name[0] ? model->target_name : "(no target)";
    canvas_draw_str(canvas, 0, 10, name);

    // TX blink box, top-right: filled when actively transmitting, outline otherwise.
    if(model->tx_on) {
        canvas_draw_box(canvas, 118, 1, 8, 6);
    } else {
        canvas_draw_frame(canvas, 118, 1, 8, 6);
    }
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);

    if(!model->have_data) {
        canvas_draw_str(canvas, 2, 30, "idle...");
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Run L:Stop R:Step D:Mark");
        return;
    }

    // Line 2: address.
    const char* addr = model->addr_str[0] ? model->addr_str : "--:--:--:--:--:--";
    canvas_draw_str(canvas, 0, 21, addr);

    // Line 3: mode label.
    canvas_draw_str(canvas, 0, 32, model->mode_str);

    // Line 4: iteration index / total + counter.
    char stat_lbl[32];
    if(model->total > 0) {
        snprintf(
            stat_lbl,
            sizeof(stat_lbl),
            "%lu/%lu  cnt:%lu",
            (unsigned long)model->index,
            (unsigned long)model->total,
            (unsigned long)model->counter);
    } else {
        snprintf(
            stat_lbl,
            sizeof(stat_lbl),
            "%lu/--  cnt:%lu",
            (unsigned long)model->index,
            (unsigned long)model->counter);
    }
    canvas_draw_str(canvas, 0, 43, stat_lbl);

    canvas_draw_line(canvas, 0, 53, 127, 53);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "OK:Run L:Stop R:Step D:Mark");
}

static bool ble_fuzz_view_input(InputEvent* event, void* context) {
    furi_assert(context);
    BleFuzzView* instance = context;
    if(event->type != InputTypeShort && event->type != InputTypeLong) return false;
    switch(event->key) {
    case InputKeyOk:
        ble_fuzz_view_fire(instance, BleFuzzViewEventStartPause);
        return true;
    case InputKeyLeft:
        ble_fuzz_view_fire(instance, BleFuzzViewEventStop);
        return true;
    case InputKeyRight:
        ble_fuzz_view_fire(instance, BleFuzzViewEventStep);
        return true;
    case InputKeyDown:
        ble_fuzz_view_fire(instance, BleFuzzViewEventMark);
        return true;
    default:
        return false; // Back (and Up) -> scene-manager navigation exits the scene
    }
}

BleFuzzView* ble_fuzz_view_alloc(void) {
    BleFuzzView* instance = malloc(sizeof(BleFuzzView));
    instance->callback = NULL;
    instance->context = NULL;
    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(BleFuzzViewModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, (ViewDrawCallback)ble_fuzz_view_draw);
    view_set_input_callback(instance->view, ble_fuzz_view_input);

    with_view_model(
        instance->view,
        BleFuzzViewModel * model,
        {
            memset(model, 0, sizeof(BleFuzzViewModel));
            model->have_data = false;
        },
        true);

    return instance;
}

void ble_fuzz_view_free(BleFuzzView* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* ble_fuzz_view_get_view(BleFuzzView* instance) {
    furi_assert(instance);
    return instance->view;
}

#endif // PUBLIC_BUILD
