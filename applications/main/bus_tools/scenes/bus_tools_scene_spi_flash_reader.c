#include "../bus_tools_app_i.h"
#include <furi_hal_spi.h>
#include <stdio.h>

typedef struct {
    uint32_t jedec_id;
    uint32_t total_size;
} SpiFlashModel;

void bus_tools_scene_spi_flash_reader_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;

    Submenu* submenu = app->submenu;
    submenu_reset(submenu);

    // Display SPI Flash Reader options
    submenu_add_item(submenu, "Read JEDEC ID", 0, NULL, app);
    submenu_add_item(submenu, "Dump to SD Card", 1, NULL, app);
    submenu_add_item(submenu, "Erase Chip", 2, NULL, app);
    submenu_add_item(submenu, "Write Test", 3, NULL, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewSubmenu);
}

bool bus_tools_scene_spi_flash_reader_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        // Handle SPI Flash operations
        // In a full implementation, would read JEDEC ID, dump to SD, etc.
        consumed = true;
    }

    return consumed;
}

void bus_tools_scene_spi_flash_reader_on_exit(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    submenu_reset(app->submenu);
}
