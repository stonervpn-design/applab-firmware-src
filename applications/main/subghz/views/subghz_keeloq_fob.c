#include "subghz_keeloq_fob.h"
#include "../helpers/subghz_fob_nav.h"
#include "../helpers/subghz_fob_buttons.h"
#include <lib/subghz/protocols/vag.h>
#include <lib/subghz/protocols/keeloq.h>
#include <gui/elements.h>
#include <furi.h>

/* Board capability flags (BOARD_TWO_BUTTON_INPUT). Guarded so this builds even
 * where BOARD_INCLUDE is undefined. */
#ifdef BOARD_INCLUDE
#include BOARD_INCLUDE
#endif

// One table-driven model. The button grid, title, counter width and the VAG
// desync note are all data now: VAG/KeeLoq are just two preset tables (see
// subghz_keeloq_fob_set_state) and every supported car protocol reaches the same
// render/nav path via subghz_keeloq_fob_set_table. Only a pointer + count are
// stored (the tables are static const from the provider) so nothing is heap-held
// on the no-PSRAM Cardputer.
typedef struct {
    uint8_t sel; // 0..n_tiles-1
    uint8_t n_tiles; // 1..SUBGHZ_KEELOQ_FOB_MAX_TILES
    uint8_t real_btn; // captured code, marks confirmed-real tile
    uint32_t cnt; // up to 24-bit for VAG
    uint16_t jump;
    bool sent;
    bool wide_cnt; // VAG 24-bit counter (%06lX) vs KeeLoq-style 16-bit (%04lX)
    bool desync_caution; // draw VAG "adv cnt / desync!" note in the empty 4th cell
    const SubGhzFobButton* buttons; // static provider table (not owned, no copy)
    const char* title; // static title string (not owned)
} SubGhzKeeloqFobModel;

// Tile rectangle for tile i of n. n<=4 reproduces the exact legacy 2x2 layout
// (60x18 boxes at the original coordinates) so VAG (3) and KeeLoq (4) render
// pixel-identically to before; n>4 packs a denser 2-column grid (up to 8 tiles).
static void fob_tile_rect(uint8_t i, uint8_t n, uint8_t* x, uint8_t* y, uint8_t* w, uint8_t* h) {
    if(n <= 4) {
        *x = (i & 1) ? 66 : 2;
        *y = (i < 2) ? 13 : 35;
        *w = 60;
        *h = 18;
    } else {
        *x = (i & 1) ? 66 : 2;
        *y = (uint8_t)(12 + (i >> 1) * 13);
        *w = 60;
        *h = 11;
    }
}

struct SubGhzKeeloqFob {
    View* view;
    SubGhzKeeloqFobCallback callback;
    void* context;
};

static void keeloq_fob_draw_callback(Canvas* canvas, void* _model) {
    SubGhzKeeloqFobModel* m = _model;
    const SubGhzFobButton* btns = m->buttons;
    uint8_t n = m->n_tiles;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 0, AlignCenter, AlignTop, m->title ? m->title : "Fob Emulator");
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < n; i++) {
        uint8_t x, y, w, h;
        fob_tile_rect(i, n, &x, &y, &w, &h);
        if(i == m->sel) {
            canvas_draw_rbox(canvas, x, y, w, h, 3);
            canvas_invert_color(canvas);
        } else {
            canvas_draw_rframe(canvas, x, y, w, h, 3);
        }
        canvas_draw_str_aligned(
            canvas, x + w / 2, y + h / 2, AlignCenter, AlignCenter, btns[i].name);
        if(m->real_btn == btns[i].code) canvas_draw_disc(canvas, x + w - 6, y + 4, 2);
        if(i == m->sel) canvas_invert_color(canvas);
    }
    // VAG has only 3 tiles; the empty 4th cell carries the desync caution.
    if(m->desync_caution) {
        canvas_draw_str_aligned(canvas, 66 + 30, 35 + 5, AlignCenter, AlignCenter, "adv cnt");
        canvas_draw_str_aligned(canvas, 66 + 30, 35 + 14, AlignCenter, AlignCenter, "desync!");
    }
    char st[36];
    // VAG counter is 24-bit (%06lX); KeeLoq is 16-bit — keep its original %04X look.
    snprintf(
        st,
        sizeof(st),
        m->wide_cnt ? "cnt %06lX  jump %u%s" : "cnt %04lX  jump %u%s",
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

// Shared internal setter: stores a static button table + presentation flags and
// derives the initial selection from the captured button. All public setters
// funnel through here so there is exactly one render/nav configuration path.
static void fob_apply_table(
    SubGhzKeeloqFob* instance,
    const SubGhzFobButton* buttons,
    size_t n,
    const char* title,
    bool wide_cnt,
    bool desync_caution,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump) {
    furi_assert(instance);
    furi_check(buttons && n > 0); // provider must have a table for a routed protocol
    if(n > SUBGHZ_KEELOQ_FOB_MAX_TILES) n = SUBGHZ_KEELOQ_FOB_MAX_TILES;
    uint8_t sel = 0;
    for(size_t i = 0; i < n; i++)
        if(buttons[i].code == real_btn) sel = (uint8_t)i;
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            m->buttons = buttons;
            m->n_tiles = (uint8_t)n;
            m->title = title;
            m->wide_cnt = wide_cnt;
            m->desync_caution = desync_caution;
            m->real_btn = real_btn;
            m->cnt = cnt;
            m->jump = jump;
            m->sel = sel;
            m->sent = false;
        },
        true);
}

SubGhzKeeloqFob* subghz_keeloq_fob_alloc(void) {
    SubGhzKeeloqFob* instance = malloc(sizeof(SubGhzKeeloqFob));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(SubGhzKeeloqFobModel));
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, keeloq_fob_draw_callback);
    view_set_input_callback(instance->view, keeloq_fob_input_callback);

    // Default to the generic KeeLoq layout so the view is valid before any
    // set_state/set_table call (matches the previous alloc-time defaults).
    size_t n = 0;
    const SubGhzFobButton* keeloq = subghz_fob_buttons(SUBGHZ_PROTOCOL_KEELOQ_NAME, &n);
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * model,
        {
            model->sel = 0;
            model->buttons = keeloq;
            model->n_tiles = (uint8_t)n; // 4
            model->title = "Fob Emulator";
            model->wide_cnt = false;
            model->desync_caution = false;
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

void subghz_keeloq_fob_set_table(
    SubGhzKeeloqFob* instance,
    const SubGhzFobButton* buttons,
    size_t n,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump) {
    // Generic protocols: default title + 16-bit counter, no VAG desync note.
    fob_apply_table(instance, buttons, n, "Fob Emulator", false, false, real_btn, cnt, jump);
}

void subghz_keeloq_fob_set_title(SubGhzKeeloqFob* instance, const char* title) {
    furi_assert(instance);
    with_view_model(instance->view, SubGhzKeeloqFobModel * m, { m->title = title; }, true);
}

void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance,
    SubGhzFobKind kind,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump) {
    size_t n = 0;
    if(kind == SubGhzFobKindVag) {
        // VAG: 3 tiles UNLOCK/LOCK/BOOT (0x1/0x2/0x4), 24-bit counter, desync note.
        const SubGhzFobButton* vag = subghz_fob_buttons(VAG_PROTOCOL_NAME, &n);
#ifndef PUBLIC_BUILD
        const char* title = "VAG Fob";
#else
        const char* title = "Fob Emulator"; // VAG (car) label stripped on public
#endif
        fob_apply_table(instance, vag, n, title, true, true, real_btn, cnt, jump);
    } else {
        // KeeLoq: 4 tiles LOCK/UNLOCK/TRUNK/PANIC (0x1/0x2/0x4/0x8), 16-bit counter.
        const SubGhzFobButton* keeloq = subghz_fob_buttons(SUBGHZ_PROTOCOL_KEELOQ_NAME, &n);
        fob_apply_table(instance, keeloq, n, "Fob Emulator", false, false, real_btn, cnt, jump);
    }
}

uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* instance) {
    uint8_t nib = 0x1;
    with_view_model(
        instance->view,
        SubGhzKeeloqFobModel * m,
        {
            if(m->buttons && m->sel < m->n_tiles) nib = m->buttons[m->sel].code;
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
