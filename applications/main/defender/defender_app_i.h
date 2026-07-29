#pragma once
#include "defender_app.h"
#include "defender_custom_event.h"
#include "scenes/defender_scene.h"
#include "defender_alert.h"
#include "defender_logger.h"
#include "defender_locator.h"
#include "defender_wifi_tap.h"
#include "views/defender_ratemeter_view.h"
#include "views/defender_waterfall_view.h"
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>

typedef enum {
    DefenderViewSubmenu,
    DefenderViewLocator,
    DefenderViewRateMeter, // T6: deauth / BLE tracker / BLE spam rate display
    DefenderViewWaterfall, // T9: SubGHz jam RSSI waterfall
    DefenderViewWidget, // rogue-AP text display
} DefenderView;

struct DefenderApp {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    View* ratemeter; // Shared rate-meter view (deauth/tracker/spam scenes).
    View* waterfall; // Shared waterfall view (jammer scene).
    Widget* widget; // Rogue-AP text display.

    // Shared components (T2..T5).
    DefenderAlertBus alert;   // Alert bus: debounce, LED/beep, ring.
    DefenderLogger* logger;   // SD event logger (NULL if SD unavailable).
    DefenderLocator* locator; // RSSI bar/beep locator view controller.
    bool wifi_tap_active;     // WiFi promiscuous tap running flag.

    // Locator handoff: a detector scene stashes an offender here and pushes
    // the Locator scene; the Locator scene reads these when locator_active.
    bool locator_active;
    char locator_label[24];
    int8_t locator_floor;
    int8_t locator_ceil;
};
