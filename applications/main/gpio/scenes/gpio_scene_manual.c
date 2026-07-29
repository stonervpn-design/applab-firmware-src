#include "../gpio_app_i.h"
#include "../gpio_pin_catalog.h"
#include "../gpio_driver.h"

#include <furi.h>

/* ~5 Hz redraw so the list's live input reads stay current. */
#define GPIO_MANUAL_REDRAW_PERIOD_MS 200

static void gpio_scene_manual_timer_cb(void* context) {
    GpioApp* app = context;
    gpio_manual_list_notify_redraw(app->manual_list);
}

void gpio_scene_manual_on_enter(void* context) {
    furi_assert(context);
    GpioApp* app = context;

    gpio_manual_list_set_context(app->manual_list, app);

    if(!app->manual_timer) {
        app->manual_timer =
            furi_timer_alloc(gpio_scene_manual_timer_cb, FuriTimerTypePeriodic, app);
    }
    furi_timer_start(app->manual_timer, furi_ms_to_ticks(GPIO_MANUAL_REDRAW_PERIOD_MS));

    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewManualList);
}

bool gpio_scene_manual_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    GpioApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        /* This handler runs only while the LIST scene is on top, so a Back here
           means we are leaving Manual Control back to the Start menu. Release
           all pins to high-Z exactly once, here (not on every editor open). */
        gpio_driver_release_all();
        return false; /* let the scene manager run previous_scene */
    }

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GpioManualEventToggleAdvanced) {
            app->show_advanced = !app->show_advanced;
            gpio_manual_list_notify_redraw(app->manual_list);
            consumed = true;
        } else if(event.event == GpioManualEventEditPin) {
            scene_manager_next_scene(app->scene_manager, GpioSceneManualEdit);
            consumed = true;
        }
    }

    return consumed;
}

void gpio_scene_manual_on_exit(void* context) {
    furi_assert(context);
    GpioApp* app = context;

    if(app->manual_timer) {
        furi_timer_stop(app->manual_timer);
        furi_timer_free(app->manual_timer);
        app->manual_timer = NULL;
    }
}
