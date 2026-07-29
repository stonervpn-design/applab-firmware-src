#include "widget_element_i.h"
#include <gui/elements.h>

/* Board capability flags (e.g. BOARD_TWO_BUTTON_INPUT). Guarded include so this
 * builds even on a board that does not set BOARD_INCLUDE. */
#ifdef BOARD_INCLUDE
#include BOARD_INCLUDE
#endif

typedef struct {
    GuiButtonType button_type;
    FuriString* text;
    ButtonCallback callback;
    void* context;
} GuiButtonModel;

static void gui_button_draw(Canvas* canvas, WidgetElement* element) {
    furi_assert(canvas);
    furi_assert(element);
    GuiButtonModel* model = element->model;

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    if(model->button_type == GuiButtonTypeLeft) {
        elements_button_left(canvas, furi_string_get_cstr(model->text));
    } else if(model->button_type == GuiButtonTypeRight) {
        elements_button_right(canvas, furi_string_get_cstr(model->text));
    } else if(model->button_type == GuiButtonTypeCenter) {
        elements_button_center(canvas, furi_string_get_cstr(model->text));
    }
}

static bool gui_button_input(InputEvent* event, WidgetElement* element) {
    GuiButtonModel* model = element->model;
    bool consumed = false;

    if(model->callback == NULL) return consumed;

    InputKey key = event->key;
#ifdef BOARD_TWO_BUTTON_INPUT
    /* Two-button board has no Left/Right key: alias Down->Left button, Ok->Right
     * button so confirmation rows ("Exit to X Menu?") are reachable. Center still
     * uses Ok. See BOARD_TWO_BUTTON_INPUT in the board header. */
    if(key == InputKeyDown) {
        key = InputKeyLeft;
    } else if(key == InputKeyOk && model->button_type == GuiButtonTypeRight) {
        key = InputKeyRight;
    }
#endif

    if((model->button_type == GuiButtonTypeLeft) && (key == InputKeyLeft)) {
        model->callback(model->button_type, event->type, model->context);
        consumed = true;
    } else if((model->button_type == GuiButtonTypeRight) && (key == InputKeyRight)) {
        model->callback(model->button_type, event->type, model->context);
        consumed = true;
    } else if((model->button_type == GuiButtonTypeCenter) && (key == InputKeyOk)) {
        model->callback(model->button_type, event->type, model->context);
        consumed = true;
    }

    return consumed;
}

static void gui_button_free(WidgetElement* gui_button) {
    furi_assert(gui_button);

    GuiButtonModel* model = gui_button->model;
    furi_string_free(model->text);
    free(gui_button->model);
    free(gui_button);
}

WidgetElement* widget_element_button_create(
    GuiButtonType button_type,
    const char* text,
    ButtonCallback callback,
    void* context) {
    // Allocate and init model
    GuiButtonModel* model = malloc(sizeof(GuiButtonModel));
    model->button_type = button_type;
    model->callback = callback;
    model->context = context;
    model->text = furi_string_alloc_set(text);

    // Allocate and init Element
    WidgetElement* gui_button = malloc(sizeof(WidgetElement));
    gui_button->parent = NULL;
    gui_button->input = gui_button_input;
    gui_button->draw = gui_button_draw;
    gui_button->free = gui_button_free;
    gui_button->model = model;

    return gui_button;
} //-V773
