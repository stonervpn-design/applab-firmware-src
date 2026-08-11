#include "../subghz_i.h"
#include "../helpers/vehicle_vuln_db.h"

#ifndef PUBLIC_BUILD
static void subghz_scene_vuln_makes_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneVulnMakes, index);
    scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnList);
}

void subghz_scene_vuln_makes_on_enter(void* context) {
    SubGhz* subghz = context;

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "Vehicle Vulns");

    for(size_t i = 0; i < vehicle_vuln_make_count(); i++) {
        const char* make = vehicle_vuln_make(i);
        if(make == NULL) continue;
        submenu_add_item(subghz->submenu, make, i, subghz_scene_vuln_makes_callback, subghz);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_vuln_makes_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_vuln_makes_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
#endif /* !PUBLIC_BUILD — vehicle vuln browser is car-only, stripped from public */
