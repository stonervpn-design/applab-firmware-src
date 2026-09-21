#include "../wlan_app.h"

enum ReconIndex {
    ReconIdxSpectrum,
    ReconIdxKarma,
    ReconIdxDetector,
    ReconIdxProbe,
    ReconIdxStations,
    ReconIdxLoot,
};

static void recon_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_recon_menu_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Recon+");
    submenu_add_item(app->submenu, "Spectrum", ReconIdxSpectrum, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Karma", ReconIdxKarma, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Attack Detector", ReconIdxDetector, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Probe Sniffer", ReconIdxProbe, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Station Scanner", ReconIdxStations, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Loot", ReconIdxLoot, recon_menu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_recon_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    switch(event.event) {
    case ReconIdxSpectrum:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneSpectrum);
        return true;
    case ReconIdxKarma:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneKarma);
        return true;
    case ReconIdxDetector:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneAttackDetector);
        return true;
    case ReconIdxProbe:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneProbeSniffer);
        return true;
    case ReconIdxStations:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneStationScanner);
        return true;
    case ReconIdxLoot:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneLootBrowser);
        return true;
    default:
        return false;
    }
}

void wlan_app_scene_recon_menu_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
