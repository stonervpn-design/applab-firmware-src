#include "../wiegand_app_i.h"
#include "../wiegand_pin_catalog.h"
#include "../wiegand_driver.h"

void wiegand_scene_replay_on_enter(void* context) {
    furi_assert(context);
    WiegandApp* app = context;

    // Initialize replay state with defaults
    app->state.d0_pin = 2;  // Default to Grove SDA
    app->state.d1_pin = 1;  // Default to Grove SCL
}

bool wiegand_scene_replay_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    WiegandApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        if (event.event == WiegandEvtCraft) {
            // Replay the stored credential
            if (app->state.bit_count > 0) {
                wiegand_driver_replay(app->state.d0_pin, app->state.d1_pin, app->state.bits, app->state.bit_count);
                consumed = true;
            }
        }
    }

    return consumed;
}

void wiegand_scene_replay_on_exit(void* context) {
    furi_assert(context);
}
