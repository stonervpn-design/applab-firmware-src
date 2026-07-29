#include "../bus_tools_app_i.h"
#include "../bus_tools_i2c_scanner.h"
#include <furi_hal_i2c.h>
#include <stdio.h>

typedef struct {
    I2cDevice devices[128];
    uint8_t device_count;
    uint8_t current_index;
} I2cScannerModel;

void bus_tools_scene_i2c_scanner_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;

    Submenu* submenu = app->submenu;
    submenu_reset(submenu);

    // Scan I2C bus (simulated - real scan would use furi_hal_i2c)
    // For this integration, we use the core to get known devices
    I2cDevice devices[128];
    uint8_t device_count = bus_tools_i2c_scan(devices, 128);

    // Display found devices
    char item_label[64];
    for(uint8_t i = 0; i < device_count; i++) {
        const char* name = bus_tools_i2c_device_name(devices[i].address);
        if(name) {
            snprintf(item_label, sizeof(item_label), "0x%02X: %s", devices[i].address, name);
        } else {
            snprintf(item_label, sizeof(item_label), "0x%02X: Unknown", devices[i].address);
        }
        submenu_add_item(submenu, item_label, i, NULL, app);
    }

    if(device_count == 0) {
        submenu_add_item(submenu, "No devices found", 0, NULL, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewSubmenu);
}

bool bus_tools_scene_i2c_scanner_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void bus_tools_scene_i2c_scanner_on_exit(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    submenu_reset(app->submenu);
}
