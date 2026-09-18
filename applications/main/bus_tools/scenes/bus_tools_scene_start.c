#include "../bus_tools_app_i.h"

static void bus_tools_scene_start_submenu_callback(void* context, uint32_t index) {
    BusToolsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void bus_tools_scene_start_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    SceneManager* scene_manager = app->scene_manager;
    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "I2C Scanner", 0, bus_tools_scene_start_submenu_callback, app);
    submenu_add_item(
        app->submenu, "UART Console", 1, bus_tools_scene_start_submenu_callback, app);
    /* "SPI Flash Reader" (index 2) hidden: it was a non-functional stub (NULL
     * callbacks) and this board has no external SPI-flash target — the only SPI
     * bus is SPI3, shared with the SD card + CC1101. Re-enable once a real target
     * + dedicated CS exists. */
    submenu_add_item(
        app->submenu, "NFC Read (ST25R)", 3, bus_tools_scene_start_submenu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewSubmenu);
}

bool bus_tools_scene_start_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    SceneManager* scene_manager = app->scene_manager;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        const uint32_t submenu_index = event.event;
        if(submenu_index == 0) {
            scene_manager_next_scene(scene_manager, BusToolsSceneI2cScanner);
        } else if(submenu_index == 1) {
            scene_manager_next_scene(scene_manager, BusToolsSceneUartConsole);
        } else if(submenu_index == 2) {
            scene_manager_next_scene(scene_manager, BusToolsSceneSpiFlashReader);
        } else if(submenu_index == 3) {
            scene_manager_next_scene(scene_manager, BusToolsSceneStNfc);
        }
        consumed = true;
    }
    return consumed;
}

void bus_tools_scene_start_on_exit(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    submenu_reset(app->submenu);
}
