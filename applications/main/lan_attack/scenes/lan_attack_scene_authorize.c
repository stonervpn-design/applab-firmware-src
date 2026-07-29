#include "../lan_attack_app_i.h"
#include <furi.h>

// Authorized-use notice. Recon generates active network traffic (ARP sweep,
// TCP connect scan, banner grab); this screen makes the operator acknowledge
// that it must only be run against networks they own or are permitted to test.
void lan_attack_scene_authorize_on_enter(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;

    widget_reset(app->widget);
    widget_add_text_scroll_element(
        app->widget,
        0,
        0,
        128,
        64,
        "\e#Authorized Use Only\e#\n"
        "Recon actively probes the LAN (ARP sweep, TCP port scan, banner grab). "
        "Run it ONLY on networks you own or have explicit written permission to "
        "test. Unauthorized scanning may be illegal.\n"
        "Press Back to return.");

    // Acknowledged for this session; recon is passive-facing but we record consent.
    app->authorized = true;
    view_dispatcher_switch_to_view(app->view_dispatcher, LanAttackViewWidget);
}

bool lan_attack_scene_authorize_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    UNUSED(context);
    UNUSED(event);
    return false; // Back is handled by the view dispatcher's navigation callback
}

void lan_attack_scene_authorize_on_exit(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;
    widget_reset(app->widget);
}
