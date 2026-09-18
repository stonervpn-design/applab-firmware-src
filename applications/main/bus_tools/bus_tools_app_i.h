#pragma once

#include "bus_tools_app.h"
#include "bus_tools_custom_event.h"
#include "scenes/bus_tools_scene.h"
#include "uart_monitor_fmt.h"
#include "bus_tools_uart_view.h"
#include "bus_tools_nfc_view.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>

typedef enum {
    BusToolsViewSubmenu,
    BusToolsViewUartMonitor,
    BusToolsViewStNfc,
    // More views added in later tasks
} BusToolsView;

struct BusToolsApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    bool advanced_unlocked;

    /* On-screen UART serial monitor (ESP-IDF UART1 on Grove pins). */
    View* uart_view;
    UartRing uart_ring;
    uint32_t uart_baud;

    /* ST25R3916 NFC-A "prove it reads" test. */
    View* nfc_view;
};
