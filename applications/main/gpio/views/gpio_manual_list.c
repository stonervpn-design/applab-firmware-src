#include "gpio_manual_list.h"
#include "../gpio_app_i.h"
#include "../gpio_pin_catalog.h"
#include "../gpio_driver.h"

#include <furi.h>
#include <gui/elements.h>

struct GpioManualList {
    View* view;
};

typedef struct {
    GpioApp* app;
    size_t selected; // 0 = Advanced row; 1..N = catalog pin (index = selected-1)
    bool confirm_advanced; // awaiting a 2nd OK to reveal shared pins
} GpioManualListModel;

static void gpio_manual_list_state_str(GpioApp* app, size_t i, char* buf, size_t n) {
    const GpioCatalogEntry* e = gpio_catalog_get(i, app->show_advanced);
    if(!e) {
        snprintf(buf, n, "--");
        return;
    }
    GpioPinState* st = &app->pin_state[i];
    switch(st->mode) {
    case GpioPinModeOutput:
        snprintf(buf, n, "OUT %s", st->out_level ? "HIGH" : "LOW");
        break;
    case GpioPinModeInput: {
        bool v = gpio_driver_read_digital(e->gpio);
        char pull = (st->pull == GpioDriverPullUp)   ? 'U' :
                    (st->pull == GpioDriverPullDown) ? 'D' :
                                                       '-';
        snprintf(buf, n, "IN %d%c", v ? 1 : 0, pull);
        break;
    }
    case GpioPinModePwm:
        snprintf(buf, n, "PWM %lu %u%%", (unsigned long)st->pwm_freq, (unsigned)st->pwm_duty);
        break;
    case GpioPinModeAdc: {
        uint32_t mv = 0;
        if(gpio_driver_read_adc(e->adc_ch, &mv)) {
            snprintf(buf, n, "ADC %lu.%02luV", (unsigned long)(mv / 1000),
                     (unsigned long)((mv % 1000) / 10));
        } else {
            snprintf(buf, n, "ADC --");
        }
        break;
    }
    default:
        snprintf(buf, n, "--");
        break;
    }
}

static void gpio_manual_list_draw_callback(Canvas* canvas, void* _model) {
    GpioManualListModel* model = _model;
    GpioApp* app = model->app;
    if(!app) return;

    size_t total = gpio_catalog_count(app->show_advanced) + 1;
    if(model->selected >= total) model->selected = total - 1;

    const int row_h = 12;
    const size_t visible = 5;
    size_t first = 0;
    if(model->selected >= visible) first = model->selected - visible + 1;

    canvas_set_font(canvas, FontSecondary);

    for(size_t r = first; r < total && r < first + visible; r++) {
        int y = (int)((r - first) * row_h);
        bool sel = (r == model->selected);
        if(sel) {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_box(canvas, 0, y, 122, row_h);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_set_color(canvas, ColorBlack);
        }
        int ty = y + 9;

        if(r == 0) {
            if(model->confirm_advanced && !app->show_advanced) {
                canvas_draw_str(canvas, 2, ty, "Reveal shared pins? OK=yes");
            } else {
                char b[24];
                snprintf(b, sizeof(b), "Advanced: [%s]", app->show_advanced ? "ON" : "OFF");
                canvas_draw_str(canvas, 2, ty, b);
            }
        } else {
            size_t i = r - 1;
            const GpioCatalogEntry* e = gpio_catalog_get(i, app->show_advanced);
            if(e) {
                canvas_draw_str(canvas, 2, ty, e->name);
                char st[16];
                gpio_manual_list_state_str(app, i, st, sizeof(st));
                uint16_t w = canvas_string_width(canvas, st);
                canvas_draw_str(canvas, 120 - w, ty, st);
            }
        }
    }

    canvas_set_color(canvas, ColorBlack);
    elements_scrollbar(canvas, model->selected, total);
}

static bool gpio_manual_list_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    GpioManualList* instance = context;

    bool is_ok = (event->key == InputKeyOk && event->type == InputTypeShort);
    bool is_up = (event->key == InputKeyUp &&
                  (event->type == InputTypeShort || event->type == InputTypeRepeat));
    bool is_down = (event->key == InputKeyDown &&
                    (event->type == InputTypeShort || event->type == InputTypeRepeat));
    if(!is_ok && !is_up && !is_down) return false;

    bool consumed = false;
    with_view_model(
        instance->view,
        GpioManualListModel * model,
        {
            GpioApp* app = model->app;
            if(app) {
                size_t total = gpio_catalog_count(app->show_advanced) + 1;
                if(model->selected >= total) model->selected = total - 1;

                if(is_up) {
                    if(model->selected > 0) model->selected--;
                    model->confirm_advanced = false;
                } else if(is_down) {
                    if(model->selected + 1 < total) model->selected++;
                    model->confirm_advanced = false;
                } else if(is_ok) {
                    if(model->selected == 0) {
                        if(app->show_advanced) {
                            /* Currently ON — turn off, no confirm needed. */
                            view_dispatcher_send_custom_event(
                                app->view_dispatcher, GpioManualEventToggleAdvanced);
                            model->confirm_advanced = false;
                        } else if(!model->confirm_advanced) {
                            /* First OK: ask for confirmation. */
                            model->confirm_advanced = true;
                        } else {
                            /* Second OK: reveal shared pins. */
                            view_dispatcher_send_custom_event(
                                app->view_dispatcher, GpioManualEventToggleAdvanced);
                            model->confirm_advanced = false;
                        }
                    } else {
                        app->edit_index = model->selected - 1;
                        model->confirm_advanced = false;
                        view_dispatcher_send_custom_event(
                            app->view_dispatcher, GpioManualEventEditPin);
                    }
                }
                consumed = true;
            }
        },
        true);

    return consumed;
}

GpioManualList* gpio_manual_list_alloc(void) {
    GpioManualList* instance = malloc(sizeof(GpioManualList));

    instance->view = view_alloc();
    view_allocate_model(instance->view, ViewModelTypeLocking, sizeof(GpioManualListModel));
    with_view_model(
        instance->view,
        GpioManualListModel * model,
        {
            model->app = NULL;
            model->selected = 0;
            model->confirm_advanced = false;
        },
        false);

    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, gpio_manual_list_draw_callback);
    view_set_input_callback(instance->view, gpio_manual_list_input_callback);

    return instance;
}

void gpio_manual_list_free(GpioManualList* instance) {
    furi_assert(instance);
    view_free(instance->view);
    free(instance);
}

View* gpio_manual_list_get_view(GpioManualList* instance) {
    furi_assert(instance);
    return instance->view;
}

void gpio_manual_list_set_context(GpioManualList* instance, GpioApp* app) {
    furi_assert(instance);
    with_view_model(
        instance->view,
        GpioManualListModel * model,
        {
            model->app = app;
            model->confirm_advanced = false;
        },
        true);
}

void gpio_manual_list_notify_redraw(GpioManualList* instance) {
    furi_assert(instance);
    with_view_model(
        instance->view, GpioManualListModel * model, { UNUSED(model); }, true);
}
