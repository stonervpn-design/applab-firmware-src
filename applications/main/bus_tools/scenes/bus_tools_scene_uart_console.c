#include "../bus_tools_app_i.h"
#include <stdio.h>

/* Baud selector for the on-screen UART monitor. Selecting a rate opens the
 * live hex/ASCII monitor (bus_tools_scene_uart_monitor). */
static const uint32_t bt_uart_bauds[] = {9600, 19200, 38400, 57600, 115200, 230400};
#define BT_UART_BAUD_COUNT (sizeof(bt_uart_bauds) / sizeof(bt_uart_bauds[0]))

static void bus_tools_uart_console_cb(void* context, uint32_t index) {
    BusToolsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void bus_tools_scene_uart_console_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "UART Baud (RX=G2)");
    char label[24];
    for(uint32_t i = 0; i < BT_UART_BAUD_COUNT; i++) {
        snprintf(label, sizeof(label), "%lu baud", (unsigned long)bt_uart_bauds[i]);
        submenu_add_item(app->submenu, label, i, bus_tools_uart_console_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewSubmenu);
}

bool bus_tools_scene_uart_console_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event < BT_UART_BAUD_COUNT) {
        app->uart_baud = bt_uart_bauds[event.event];
        scene_manager_next_scene(app->scene_manager, BusToolsSceneUartMonitor);
        return true;
    }
    return false;
}

void bus_tools_scene_uart_console_on_exit(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    submenu_reset(app->submenu);
}
