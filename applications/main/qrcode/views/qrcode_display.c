#include "qrcode_display.h"
#include "../lib/qrcode.h"

#include <furi.h>
#include <string.h>

/* Version 8 => 49x49 modules, which still fits the 64px-high screen at 1px per
 * module. Higher versions won't render legibly, so cap encoding here. */
#define QRCODE_MAX_VERSION 8
#define QRCODE_MODULE_COUNT (4 * QRCODE_MAX_VERSION + 17)
#define QRCODE_MODULES_BUF_SIZE \
    ((QRCODE_MODULE_COUNT * QRCODE_MODULE_COUNT + 7) / 8)

/* Quiet-zone / margin around the QR block, in screen pixels. */
#define QRCODE_MARGIN 2

/* Maximum byte-mode payload length at ECC_MEDIUM, indexed by (version - 1).
 * The vendored encoder does NOT bounds-check byte input (its own "@TODO:
 * Return error if data is too big"), so we must reject payloads that would
 * overflow the codeword buffer before calling qrcode_initBytes. Values taken
 * from the upstream Binary/Medium capacity table. */
static const uint16_t QRCODE_MAX_BYTE_MEDIUM[QRCODE_MAX_VERSION] = {
    14,
    26,
    42,
    62,
    84,
    106,
    122,
    152,
};

struct QRCodeDisplay {
    View* view;
};

typedef struct {
    QRCode qrcode;
    uint8_t modules[QRCODE_MODULES_BUF_SIZE];
    bool valid; // encoding succeeded within QRCODE_MAX_VERSION
} QRCodeDisplayModel;

static void qrcode_display_draw_callback(Canvas* canvas, void* _model) {
    QRCodeDisplayModel* model = _model;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    uint8_t width = canvas_width(canvas);
    uint8_t height = canvas_height(canvas);

    uint8_t size = model->qrcode.size;

    /* Guard BEFORE any division: a payload too long to encode leaves size == 0
     * (qrcode_initBytes never ran), and dividing by 0 raises IntegerDivideByZero
     * on Xtensa -> firmware panic. Bail out to the message first. */
    if(!model->valid || size == 0) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, width / 2, height / 2, AlignCenter, AlignCenter, "Too long for screen QR");
        return;
    }

    uint8_t pixel_w = (width - 2 * QRCODE_MARGIN) / size;
    uint8_t pixel_h = (height - 2 * QRCODE_MARGIN) / size;
    uint8_t pixel_size = pixel_w < pixel_h ? pixel_w : pixel_h;

    /* Encoded but too dense to fit the screen at >=1px per module. size > 0 here,
     * so the divisions above were safe. */
    if(pixel_size < 1) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, width / 2, height / 2, AlignCenter, AlignCenter, "Too long for screen QR");
        return;
    }

    uint8_t block = pixel_size * size;
    uint8_t left = (width - block) / 2;
    uint8_t top = (height - block) / 2;

    /* White background (with quiet zone) so the code reads on any prior frame. */
    canvas_set_color(canvas, ColorWhite);
    int16_t bg_x = (int16_t)left - QRCODE_MARGIN;
    int16_t bg_y = (int16_t)top - QRCODE_MARGIN;
    if(bg_x < 0) bg_x = 0;
    if(bg_y < 0) bg_y = 0;
    canvas_draw_box(
        canvas, bg_x, bg_y, block + 2 * QRCODE_MARGIN, block + 2 * QRCODE_MARGIN);

    /* Black modules. */
    canvas_set_color(canvas, ColorBlack);
    for(uint8_t y = 0; y < size; y++) {
        for(uint8_t x = 0; x < size; x++) {
            if(qrcode_getModule(&model->qrcode, x, y)) {
                if(pixel_size == 1) {
                    canvas_draw_dot(canvas, left + x, top + y);
                } else {
                    canvas_draw_box(
                        canvas, left + x * pixel_size, top + y * pixel_size, pixel_size, pixel_size);
                }
            }
        }
    }
}

static bool qrcode_display_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    /* Return false so Back/others fall through to the view dispatcher
     * navigation callback (returns to the previous scene). */
    return false;
}

void qrcode_display_set_payload(QRCodeDisplay* instance, const char* text) {
    furi_assert(instance);
    furi_assert(text);

    with_view_model(
        instance->view,
        QRCodeDisplayModel * model,
        {
            uint16_t len = (uint16_t)strlen(text);
            model->valid = false;
            for(uint8_t version = 1; version <= QRCODE_MAX_VERSION; version++) {
                /* Reject before encoding if the data can't fit this version;
                 * the encoder itself would overflow rather than report it. */
                if(len > QRCODE_MAX_BYTE_MEDIUM[version - 1]) {
                    continue;
                }
                int8_t res = qrcode_initBytes(
                    &model->qrcode,
                    model->modules,
                    MODE_BYTE,
                    version,
                    ECC_MEDIUM,
                    (uint8_t*)text,
                    len);
                if(res == 0) {
                    model->valid = true;
                    break;
                }
            }
        },
        true);
}

QRCodeDisplay* qrcode_display_alloc(void) {
    QRCodeDisplay* instance = malloc(sizeof(QRCodeDisplay));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(QRCodeDisplayModel));
    with_view_model(
        instance->view,
        QRCodeDisplayModel * model,
        {
            model->valid = false;
            model->qrcode.size = 0;
        },
        false);

    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, qrcode_display_draw_callback);
    view_set_input_callback(instance->view, qrcode_display_input_callback);

    return instance;
}

void qrcode_display_free(QRCodeDisplay* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* qrcode_display_get_view(QRCodeDisplay* instance) {
    furi_assert(instance);
    return instance->view;
}
