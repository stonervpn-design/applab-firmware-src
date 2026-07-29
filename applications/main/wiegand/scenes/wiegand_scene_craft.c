#include "../wiegand_app_i.h"
#include "../wiegand_core.h"
#include <stdlib.h>
#include <string.h>

void wiegand_scene_craft_on_enter(void* context) {
    furi_assert(context);
    WiegandApp* app = context;

    // Initialize craft state with defaults
    app->state.facility = 1;
    app->state.card = 1;
    app->state.bit_count = 0;
}

bool wiegand_scene_craft_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    WiegandApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        if (event.event == WiegandEvtCraft) {
            // Encode 26-bit credential
            if (app->state.facility <= 255 && app->state.card <= 65535) {
                app->state.bit_count = wiegand_encode_26(app->state.facility, app->state.card, app->state.bits);
                consumed = true;
            }
        }
    }

    return consumed;
}

void wiegand_scene_craft_on_exit(void* context) {
    furi_assert(context);
}
