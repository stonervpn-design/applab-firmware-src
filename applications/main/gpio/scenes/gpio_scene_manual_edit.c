#include "../gpio_app_i.h"

#include <furi.h>

void gpio_scene_manual_edit_on_enter(void* context) {
    furi_assert(context);
    GpioApp* app = context;
    gpio_manual_edit_set_context(app->manual_edit, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewManualEdit);
}

bool gpio_scene_manual_edit_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    GpioApp* app = context;
    bool consumed = false;

    /* The view_dispatcher tick (100 ms) drives the live Read row here, since the
       list scene's redraw timer is stopped while the editor is on top. */
    if(event.type == SceneManagerEventTypeTick) {
        gpio_manual_edit_tick(app->manual_edit);
        consumed = true;
    }

    return consumed;
}

void gpio_scene_manual_edit_on_exit(void* context) {
    /* Pin keeps its state until Manual Control is left entirely (handled by
       gpio_scene_manual_on_exit -> gpio_driver_release_all()). */
    UNUSED(context);
}
