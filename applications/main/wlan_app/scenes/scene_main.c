#include "../wlan_app.h"

enum MainIndex {
    MainIndexSelectWifi,
    MainIndexAttack,
    MainIndexSwitchWifi,
    MainIndexDisconnect,
    // Grouped actions below the separator (always visible).
    MainIndexChannelAttacks = 10,
    MainIndexReconPlus = 11,
    MainIndexUpdateSd = 12,
};

static void wlan_app_scene_main_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_main_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "WiFi");

    // Entering the hub clears channel mode; the connection actions below are
    // state-dependent, the grouped actions are always shown.
    app->channel_mode_active = false;
    // Hub scene: reset any Update-SD flow that was aborted earlier.
    app->update_sd_flow = false;

    if(!app->connected && !app->target_selected) {
        submenu_add_item_centered(
            app->submenu, "Select Wifi", MainIndexSelectWifi, wlan_app_scene_main_submenu_cb, app);
    } else if(app->connected) {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->connected_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Disconnect", MainIndexDisconnect, wlan_app_scene_main_submenu_cb, app);
    } else {
        char label[48];
        snprintf(label, sizeof(label), "%s Attack", app->target_ap.ssid);
        submenu_add_item(
            app->submenu, label, MainIndexAttack, wlan_app_scene_main_submenu_cb, app);
        submenu_add_item(
            app->submenu, "Switch Wifi", MainIndexSwitchWifi, wlan_app_scene_main_submenu_cb, app);
    }

    submenu_add_separator(app->submenu);

    submenu_add_item(
        app->submenu, "Channel Attacks", MainIndexChannelAttacks,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Recon+", MainIndexReconPlus,
        wlan_app_scene_main_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Update SD", MainIndexUpdateSd,
        wlan_app_scene_main_submenu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_main_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MainIndexSelectWifi:
        case MainIndexSwitchWifi:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
            consumed = true;
            break;
        case MainIndexAttack:
            if(app->connected) {
                // First entry after connect/disconnect runs the ARP scan; later
                // entries go straight to the LAN list until a re-scan is triggered.
                if(app->lan_scan_complete) {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneLan);
                } else {
                    scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkScanning);
                }
            } else {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkActions);
            }
            consumed = true;
            break;
        case MainIndexDisconnect:
            // State-only toggle back to the disconnected hub.
            app->connected = false;
            app->target_selected = false;
            app->lan_scan_complete = false;
            memset(&app->connected_ap, 0, sizeof(app->connected_ap));
            wlan_app_scene_main_on_exit(app);
            wlan_app_scene_main_on_enter(app);
            consumed = true;
            break;
        case MainIndexChannelAttacks:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneChannelAttacks);
            consumed = true;
            break;
        case MainIndexReconPlus:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneReconMenu);
            consumed = true;
            break;
        case MainIndexUpdateSd:
            app->update_sd_flow = true;
            if(app->connected) {
                // Already connected: go straight to the confirm dialog.
                scene_manager_next_scene(app->scene_manager, WlanAppSceneUpdateSd);
            } else {
                // No WiFi yet: same connect flow as "Select Wifi".
                scene_manager_next_scene(app->scene_manager, WlanAppSceneConnect);
            }
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wlan_app_scene_main_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
