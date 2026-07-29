#include "../wiegand_app_i.h"
#include "../wiegand_pin_catalog.h"
#include "../wiegand_driver.h"
#include "../wiegand_core.h"

void wiegand_scene_capture_on_enter(void* context) {
    furi_assert(context);
    WiegandApp* app = context;

    // Initialize capture state
    app->state.d0_pin = 2;  // Default to Grove SDA
    app->state.d1_pin = 1;  // Default to Grove SCL
    app->state.bit_count = 0;
}

bool wiegand_scene_capture_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    WiegandApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        if (event.event == WiegandEvtCapture) {
            // Start capture with selected pins
            wiegand_driver_start_capture(app->state.d0_pin, app->state.d1_pin, 5000);
            consumed = true;
        }
    } else if (event.type == SceneManagerEventTypeTick) {
        int32_t bits_read = wiegand_driver_read_capture(app->state.bits, 34);
        if (bits_read > 0) {
            app->state.bit_count = bits_read;
            consumed = true;
        }
    } else if (event.type == SceneManagerEventTypeBack) {
        wiegand_driver_stop_capture();
        consumed = false;
    }

    return consumed;
}

void wiegand_scene_capture_on_exit(void* context) {
    furi_assert(context);
    WiegandApp* app = context;
    wiegand_driver_stop_capture();
}
