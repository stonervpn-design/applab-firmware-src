#include "../wlan_app.h"

// Channel-wide (target-agnostic) attacks, grouped out of the main menu to keep
// the top screen short. Each entry reproduces exactly what the main menu did
// before the grouping: Capture Handshake / Deauth / Sniffer arm channel mode
// (scenes ignore target_ap and work on channel_action_channel), while SSID Spam
// and Evil Portal stay target-independent as before.
enum ChannelAttacksIndex {
    ChanAtkHandshake,
    ChanAtkDeauth,
    ChanAtkSniffer,
    ChanAtkSsidSpam,
    ChanAtkEvilPortal,
#ifndef PUBLIC_BUILD
    ChanAtkMgmtFuzzer,
#endif
};

static void channel_attacks_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_channel_attacks_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Channel Attacks");

    submenu_add_item(
        app->submenu, "Capture Handshake", ChanAtkHandshake, channel_attacks_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Deauth", ChanAtkDeauth, channel_attacks_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Sniffer", ChanAtkSniffer, channel_attacks_submenu_cb, app);
    submenu_add_item(
        app->submenu, "SSID Spam", ChanAtkSsidSpam, channel_attacks_submenu_cb, app);
    submenu_add_item(
        app->submenu, "Evil Portal", ChanAtkEvilPortal, channel_attacks_submenu_cb, app);
#ifndef PUBLIC_BUILD
    submenu_add_item(
        app->submenu, "Mgmt Fuzzer", ChanAtkMgmtFuzzer, channel_attacks_submenu_cb, app);
#endif

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_channel_attacks_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case ChanAtkHandshake:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshake);
            consumed = true;
            break;
        case ChanAtkDeauth:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppSceneNetworkDeauth);
            consumed = true;
            break;
        case ChanAtkSniffer:
            app->channel_mode_active = true;
            if(app->channel_action_channel == 0) app->channel_action_channel = 1;
            scene_manager_next_scene(app->scene_manager, WlanAppScenePackageSniffer);
            consumed = true;
            break;
        case ChanAtkSsidSpam:
            // Target-independent beacon frames -> no channel mode (as before).
            scene_manager_next_scene(app->scene_manager, WlanAppSceneSsidSpam);
            consumed = true;
            break;
        case ChanAtkEvilPortal:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneEvilPortalMenu);
            consumed = true;
            break;
#ifndef PUBLIC_BUILD
        case ChanAtkMgmtFuzzer:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneMgmtFuzzerConfig);
            consumed = true;
            break;
#endif
        }
    }

    return consumed;
}

void wlan_app_scene_channel_attacks_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
