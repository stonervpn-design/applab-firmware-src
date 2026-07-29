#include "../lan_attack_app_i.h"
#include <furi.h>

static void lan_attack_scene_main_submenu_callback(void* context, uint32_t index) {
    LanAttackApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

void lan_attack_scene_main_on_enter(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "LAN Attack");

    submenu_add_item(app->submenu, "Recon", LanAttackEvtRecon, lan_attack_scene_main_submenu_callback, app);
    submenu_add_item(app->submenu, "Authorized Use", LanAttackEvtAuthorize, lan_attack_scene_main_submenu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, LanAttackViewSubmenu);
}

bool lan_attack_scene_main_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    LanAttackApp* app = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom) {
        switch (event.event) {
        case LanAttackEvtRecon:
            scene_manager_next_scene(app->scene_manager, LanAttackSceneRecon);
            consumed = true;
            break;
        case LanAttackEvtAuthorize:
            scene_manager_next_scene(app->scene_manager, LanAttackSceneAuthorize);
            consumed = true;
            break;
        default:
            consumed = true;
            break;
        }
    }

    return consumed;
}

void lan_attack_scene_main_on_exit(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;
    submenu_reset(app->submenu);
}
