#include "subghz_keeloq_fob.h"
#include "../helpers/subghz_fob_nav.h"
#include <gui/elements.h>
#include <furi.h>

static const uint8_t FOB_CODES[4] = {0x1, 0x2, 0x4, 0x8};
static const char* const FOB_LABELS[4] = {"LOCK", "UNLOCK", "TRUNK", "PANIC"};

typedef struct {
    uint8_t sel; // 0..3
    uint8_t real_btn; // captured nibble, marks confirmed-real tile
    uint16_t cnt;
    uint16_t jump;
    bool sent;
} SubGhzKeeloqFobModel;

struct SubGhzKeeloqFob {
    View* view;
    SubGhzKeeloqFobCallback callback;
    void* context;
};

static void keeloq_fob_draw_callback(Canvas* canvas, void* _model) {
    SubGhzKeeloqFobModel* m = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Fob Emulator");
    const uint8_t rx[4] = {2, 66, 2, 66};
    const uint8_t ry[4] = {13, 13, 35, 35};
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t x = rx[i], y = ry[i];
        if(i == m->sel) {
            canvas_draw_rbox(canvas, x, y, 60, 18, 3);
            canvas_invert_color(canvas);
        } else {
            canvas_draw_rframe(canvas, x, y, 60, 18, 3);
        }
        canvas_draw_str_aligned(canvas, x + 30, y + 9, AlignCenter, AlignCenter, FOB_LABELS[i]);
        if(m->real_btn == FOB_CODES[i]) canvas_draw_disc(canvas, x + 54, y + 4, 2);
        if(i == m->sel) canvas_invert_color(canvas);
    }
    char st[32];
    snprintf(st, sizeof(st), "cnt %04X  jump %u%s", m->cnt, m->jump, m->sent ? "  SENT" : "");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, st);
}

static bool keeloq_fob_input_callback(InputEvent* event, void* context) {
    SubGhzKeeloqFob* inst = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    uint8_t navkey = 0xFF;
    switch(event->key) {
    case InputKeyLeft:
        navkey = FOB_LEFT;
        break;
    case InputKeyRight:
        navkey = FOB_RIGHT;
        break;
    case InputKeyUp:
        navkey = FOB_UP;
        break;
    case InputKeyDown:
        navkey = FOB_DOWN;
        break;
    case InputKeyOk:
        with_view_model(inst->view, SubGhzKeeloqFobModel * m, { m->sent = false; }, false);
        if(inst->callback) inst->callback(SubGhzCustomEventViewKeeloqFobSend, inst->context);
        return true;
    case InputKeyBack:
        if(inst->callback) inst->callback(SubGhzCustomEventViewKeeloqFobBack, inst->context);
        return true;
    default:
        return false;
    }
    if(navkey != 0xFF) {
        with_view_model(
            inst->view,
            SubGhzKeeloqFobModel * m,
            { m->sel = subghz_fob_nav_move(m->sel, navkey); },
            true);
        return true;
    }
    return false;
}

SubGhzKeeloqFob* subghz_keeloq_fob_alloc(void) {
    SubGhzKeeloqFob* instance = malloc(sizeof(SubGhzKeeloqFob));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzKeeloqFobModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, keeloq_fob_draw_callback);
    view_set_input_callback(instance->view, keeloq_fob_input_callback);

    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * model,
        {
            model->sel = 0;
            model->real_btn = 0;
            model->cnt = 0;
            model->jump = 100;
            model->sent = false;
        },
        true);

    instance->callback = NULL;
    instance->context = NULL;

    return instance;
}

void subghz_keeloq_fob_free(SubGhzKeeloqFob* instance) {
    furi_assert(instance);

    view_free(instance->view);
    free(instance);
}

View* subghz_keeloq_fob_get_view(SubGhzKeeloqFob* instance) {
    furi_assert(instance);
    return instance->view;
}

void subghz_keeloq_fob_set_callback(
    SubGhzKeeloqFob* instance,
    SubGhzKeeloqFobCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance,
    uint8_t real_btn,
    uint16_t cnt,
    uint16_t jump) {
    uint8_t sel = 0;
    for(uint8_t i = 0; i < 4; i++)
        if(FOB_CODES[i] == real_btn) sel = i;
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            m->real_btn = real_btn;
            m->cnt = cnt;
            m->jump = jump;
            m->sel = sel;
            m->sent = false;
        },
        true);
}

uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* instance) {
    uint8_t nib = 0x1;
    with_view_model(instance->view, SubGhzKeeloqFobModel * m, { nib = FOB_CODES[m->sel]; }, false);
    return nib;
}

void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* instance, uint16_t new_cnt) {
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            m->cnt = new_cnt;
            m->sent = true;
        },
        true);
}
