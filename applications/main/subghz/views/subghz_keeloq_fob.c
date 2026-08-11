#include "subghz_keeloq_fob.h"
#include "../helpers/subghz_fob_nav.h"
#include <gui/elements.h>
#include <furi.h>

/* Board capability flags (BOARD_TWO_BUTTON_INPUT). Guarded so this builds even
 * where BOARD_INCLUDE is undefined. */
#ifdef BOARD_INCLUDE
#include BOARD_INCLUDE
#endif

static const uint8_t FOB_CODES[4] = {0x1, 0x2, 0x4, 0x8};
static const char* const FOB_LABELS[4] = {"LOCK", "UNLOCK", "TRUNK", "PANIC"};
// VAG (AUT64): 3 buttons, codes per vag_button_name (0x1=Unlock,0x2=Lock,0x4=Boot).
static const uint8_t VAG_CODES[3] = {0x1, 0x2, 0x4};
static const char* const VAG_LABELS[3] = {"UNLOCK", "LOCK", "BOOT"};

typedef struct {
    uint8_t sel; // 0..n_tiles-1
    uint8_t kind; // SubGhzFobKind
    uint8_t n_tiles; // 4 = KeeLoq, 3 = VAG
    uint8_t real_btn; // captured code, marks confirmed-real tile
    uint32_t cnt; // up to 24-bit for VAG
    uint16_t jump;
    bool sent;
} SubGhzKeeloqFobModel;

// Active button table for a fob kind. Returns tile count; fills code/label ptrs
// (labels may be NULL when only codes are needed).
static uint8_t fob_tiles(uint8_t kind, const uint8_t** codes, const char* const** labels) {
    if(kind == SubGhzFobKindVag) {
        *codes = VAG_CODES;
        if(labels) *labels = VAG_LABELS;
        return 3;
    }
    *codes = FOB_CODES;
    if(labels) *labels = FOB_LABELS;
    return 4;
}

struct SubGhzKeeloqFob {
    View* view;
    SubGhzKeeloqFobCallback callback;
    void* context;
};

static void keeloq_fob_draw_callback(Canvas* canvas, void* _model) {
    SubGhzKeeloqFobModel* m = _model;
    const uint8_t* codes;
    const char* const* labels;
    uint8_t n = fob_tiles(m->kind, &codes, &labels);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas,
        64,
        0,
        AlignCenter,
        AlignTop,
#ifndef PUBLIC_BUILD
        m->kind == SubGhzFobKindVag ? "VAG Fob" : "Fob Emulator");
#else
        "Fob Emulator"); // VAG (car) label stripped on public
#endif
    const uint8_t rx[4] = {2, 66, 2, 66};
    const uint8_t ry[4] = {13, 13, 35, 35};
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < n; i++) {
        uint8_t x = rx[i], y = ry[i];
        if(i == m->sel) {
            canvas_draw_rbox(canvas, x, y, 60, 18, 3);
            canvas_invert_color(canvas);
        } else {
            canvas_draw_rframe(canvas, x, y, 60, 18, 3);
        }
        canvas_draw_str_aligned(canvas, x + 30, y + 9, AlignCenter, AlignCenter, labels[i]);
        if(m->real_btn == codes[i]) canvas_draw_disc(canvas, x + 54, y + 4, 2);
        if(i == m->sel) canvas_invert_color(canvas);
    }
    // VAG has only 3 tiles; the empty 4th cell carries the desync caution.
    if(m->kind == SubGhzFobKindVag) {
        canvas_draw_str_aligned(canvas, 66 + 30, 35 + 5, AlignCenter, AlignCenter, "adv cnt");
        canvas_draw_str_aligned(canvas, 66 + 30, 35 + 14, AlignCenter, AlignCenter, "desync!");
    }
    char st[36];
    // VAG counter is 24-bit (%06lX); KeeLoq is 16-bit — keep its original %04X look.
    snprintf(
        st,
        sizeof(st),
        m->kind == SubGhzFobKindVag ? "cnt %06lX  jump %u%s" : "cnt %04lX  jump %u%s",
        (unsigned long)m->cnt,
        m->jump,
        m->sent ? "  SENT" : "");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, st);
}

static bool keeloq_fob_input_callback(InputEvent* event, void* context) {
    SubGhzKeeloqFob* inst = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    uint8_t navkey = 0xFF;
    switch(event->key) {
#ifdef BOARD_TWO_BUTTON_INPUT
    // M5Stick S3 (2-button): side-button tap = InputKeyDown, double-tap = InputKeyUp —
    // both cycle to the next tile. The front button (InputKeyOk) sends; Back exits.
    case InputKeyDown:
    case InputKeyUp:
        navkey = FOB_CYCLE;
        break;
#else
    // Cardputer / full keyboard: arrows move the highlight spatially.
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
#endif
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
            { m->sel = subghz_fob_nav_move_n(m->sel, navkey, m->n_tiles); },
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
            model->kind = SubGhzFobKindKeeloq;
            model->n_tiles = 4;
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
    SubGhzFobKind kind,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump) {
    const uint8_t* codes;
    const char* const* labels;
    uint8_t n = fob_tiles((uint8_t)kind, &codes, &labels);
    uint8_t sel = 0;
    for(uint8_t i = 0; i < n; i++)
        if(codes[i] == real_btn) sel = i;
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            m->kind = (uint8_t)kind;
            m->n_tiles = n;
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
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            const uint8_t* codes;
            fob_tiles(m->kind, &codes, NULL);
            nib = codes[m->sel];
        },
        false);
    return nib;
}

void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* instance, uint32_t new_cnt) {
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            m->cnt = new_cnt;
            m->sent = true;
        },
        true);
}
