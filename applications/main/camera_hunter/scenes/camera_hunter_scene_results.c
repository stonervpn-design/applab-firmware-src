#include "../camera_hunter_app_i.h"
#include <furi.h>
#include <stdio.h>
#include <string.h>

// Results: a list of findings; selecting one opens a detail card. Scene state
// 0 = list, 1 = detail.

static void camera_hunter_results_cb(void* context, uint32_t index) {
    CameraHunterApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

static void camera_hunter_results_build_list(CameraHunterApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Cams & devices");

    uint8_t n = cam_hunt_finding_count(app->hunt);
    if(n == 0) {
        submenu_add_item(app->submenu, "No cameras found", 250, camera_hunter_results_cb, app);
    } else {
        for(uint8_t i = 0; i < n; i++) {
            const CamFinding* f = cam_hunt_finding(app->hunt, i);
            char label[64];
            if(f->kind == CamKindDetected) {
                // "~ Wyze ee:ff -52" — seen on the air, not audited.
                const char* tail = (strlen(f->mac) >= 5) ? (f->mac + strlen(f->mac) - 5) : f->mac;
                snprintf(label, sizeof(label), "~ %s %s %ddB", f->vendor, tail, (int)f->rssi);
            } else {
                snprintf(
                    label, sizeof(label), "%s %s:%u %s", f->cracked ? "[+]" : "[-]", f->ip,
                    (unsigned)f->port, f->proto);
            }
            submenu_add_item(app->submenu, label, i, camera_hunter_results_cb, app);
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewSubmenu);
}

// "Audit this camera" pressed on an own-AP camera's detail card.
static void
    camera_hunter_results_audit_cb(GuiButtonType result, InputType type, void* context) {
    CameraHunterApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        scene_manager_handle_custom_event(app->scene_manager, CamHunterEvtAuditCam);
    }
}

static void camera_hunter_results_show_detail(CameraHunterApp* app, uint8_t idx) {
    const CamFinding* f = cam_hunt_finding(app->hunt, idx);
    if(!f) return;

    char body[256];
    bool can_audit = false;
    if(f->kind == CamKindDetected) {
        if(f->is_ap && f->ssid[0]) {
            // Camera on its OWN AP: auditable on demand (connect + default creds).
            can_audit = true;
            snprintf(
                body,
                sizeof(body),
                "%s\n"
                "MAC: %s\n"
                "AP: %s\n"
                "Signal: %ddB  ch %u\n"
                "Own AP - Audit connects\n& tries default creds.",
                f->vendor,
                f->mac,
                f->ssid,
                (int)f->rssi,
                (unsigned)f->channel);
        } else {
            // Client on someone's network: can't reach it without joining that net.
            snprintf(
                body,
                sizeof(body),
                "%s\n"
                "MAC: %s\n"
                "SSID: %s\n"
                "Signal: %ddB  ch %u\n"
                "Client on a net - can't\naudit without its network.",
                f->vendor,
                f->mac,
                f->ssid[0] ? f->ssid : "(client on a net)",
                (int)f->rssi,
                (unsigned)f->channel);
        }
    } else {
        snprintf(
            body,
            sizeof(body),
            "%s:%u %s\n"
            "Vendor: %s\n"
            "Access: %s\n"
            "%s",
            f->ip,
            (unsigned)f->port,
            f->proto,
            f->vendor,
            f->cracked ? f->cred : "locked (no default cred)",
            f->url);
    }

    widget_reset(app->widget);
    if(can_audit) {
        widget_add_text_scroll_element(app->widget, 0, 0, 128, 52, body);
        widget_add_button_element(
            app->widget, GuiButtonTypeCenter, "Audit", camera_hunter_results_audit_cb, app);
    } else {
        widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, body);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, CamHunterViewWidget);
}

void camera_hunter_scene_results_on_enter(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    scene_manager_set_scene_state(app->scene_manager, CameraHunterSceneResults, 0);
    camera_hunter_results_build_list(app);
}

bool camera_hunter_scene_results_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CameraHunterApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == CamHunterEvtAuditCam) {
            // Audit the finding whose detail is showing (result_index already set).
            scene_manager_next_scene(app->scene_manager, CameraHunterSceneAudit);
        } else if(
            event.event < CAM_HUNT_MAX_FINDINGS && event.event < cam_hunt_finding_count(app->hunt)) {
            app->result_index = (uint8_t)event.event;
            scene_manager_set_scene_state(app->scene_manager, CameraHunterSceneResults, 1);
            camera_hunter_results_show_detail(app, app->result_index);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        uint32_t state =
            scene_manager_get_scene_state(app->scene_manager, CameraHunterSceneResults);
        if(state == 1) {
            // Detail -> back to the list.
            scene_manager_set_scene_state(app->scene_manager, CameraHunterSceneResults, 0);
            camera_hunter_results_build_list(app);
        } else {
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, CameraHunterSceneMain);
        }
        consumed = true;
    }

    return consumed;
}

void camera_hunter_scene_results_on_exit(void* context) {
    furi_assert(context);
    CameraHunterApp* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
}
