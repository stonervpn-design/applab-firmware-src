#pragma once
#include <furi.h>
#include "lan_attack_app.h"
#include "lan_attack_custom_event.h"
#include "scenes/lan_attack_scene.h"
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include "lan_attack_recon.h"

typedef enum {
    LanAttackViewSubmenu,
    LanAttackViewScanStatus,
    LanAttackViewWidget,
} LanAttackView;

struct LanAttackApp {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    View* view_scan_status;         // LAN recon live-status view
    Widget* widget;                 // authorized-use notice screen
    LanAttackReconState* recon;     // active recon worker (NULL when idle)
    bool authorized;  // authorization flag (session-scoped, reset to false on exit)
};
