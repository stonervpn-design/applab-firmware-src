#include "../lan_attack_app_i.h"
#include "../lan_attack_recon.h"
#include "../views/lan_attack_view_scan_status.h"
#include <furi.h>
#include <string.h>

// Extract the file name from a full path for compact display.
static const char* recon_basename(const char* path) {
    if(!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void lan_attack_scene_recon_refresh(LanAttackApp* app) {
    if(!app->recon) return;
    LanAttackReconStatus s = lan_attack_recon_status(app->recon);
    const char* csv = s.done ? recon_basename(lan_attack_recon_csv_path(app->recon)) : "";
    lan_attack_view_scan_status_update(
        app->view_scan_status,
        s.scan_progress_pct,
        s.hosts_alive,
        254,
        s.ports_found,
        s.elapsed_sec,
        s.done,
        csv);
}

void lan_attack_scene_recon_on_enter(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;

    // Reset the view and kick off the worker (auto-detects the local /24).
    lan_attack_view_scan_status_update(app->view_scan_status, 0, 0, 254, 0, 0, false, "");
    app->recon = lan_attack_recon_start(NULL, 0);

    view_dispatcher_switch_to_view(app->view_dispatcher, LanAttackViewScanStatus);
}

bool lan_attack_scene_recon_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    LanAttackApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        lan_attack_scene_recon_refresh(app);
        consumed = true;
    }

    return consumed;
}

void lan_attack_scene_recon_on_exit(void* context) {
    furi_assert(context);
    LanAttackApp* app = context;

    // Cancel + join + free the worker; sockets are closed inside the worker.
    if(app->recon) {
        lan_attack_recon_cancel(app->recon);
        lan_attack_recon_free(app->recon);
        app->recon = NULL;
    }
}
