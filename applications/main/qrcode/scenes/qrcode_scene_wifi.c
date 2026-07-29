#include "../qrcode_app_i.h"

#include <furi.h>

enum QRCodeWifiIndex {
    QRCodeWifiIndexSsid,
    QRCodeWifiIndexPass,
    QRCodeWifiIndexAuth,
    QRCodeWifiIndexGenerate,
};

static const char* const wifi_auth_labels[QRCodeWifiAuthCount] = {"WPA", "WEP", "Open"};

/** WiFi QR spec token for the T: field. Open networks use "nopass". */
static const char* qrcode_wifi_auth_token(uint8_t auth) {
    switch(auth) {
    case QRCodeWifiAuthWep:
        return "WEP";
    case QRCodeWifiAuthOpen:
        return "nopass";
    case QRCodeWifiAuthWpa:
    default:
        return "WPA";
    }
}

/** Copy src into dst escaping the WiFi QR special chars ( \ ; , : " ) with a
 *  leading backslash. Always NUL-terminates; truncates safely if dst fills. */
static void qrcode_wifi_escape(char* dst, size_t dst_size, const char* src) {
    if(dst_size == 0) return;
    size_t o = 0;
    for(size_t i = 0; src[i] != '\0' && o + 1 < dst_size; i++) {
        char c = src[i];
        if(c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
            if(o + 2 >= dst_size) break; /* no room for escape + char */
            dst[o++] = '\\';
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

static void qrcode_wifi_auth_change_cb(VariableItem* item) {
    QRCodeApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= QRCodeWifiAuthCount) idx = QRCodeWifiAuthCount - 1;
    app->wifi_auth = idx;
    variable_item_set_current_value_text(item, wifi_auth_labels[idx]);
}

static void qrcode_wifi_enter_cb(void* context, uint32_t index) {
    QRCodeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void qrcode_scene_wifi_on_enter(void* context) {
    QRCodeApp* app = context;
    VariableItemList* list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(list);
    variable_item_list_set_header(list, "WiFi");

    item = variable_item_list_add(list, "SSID", 1, NULL, app);
    variable_item_set_current_value_text(item, app->ssid[0] ? app->ssid : "(empty)");

    item = variable_item_list_add(list, "Password", 1, NULL, app);
    variable_item_set_current_value_text(item, app->pass[0] ? app->pass : "(empty)");

    item = variable_item_list_add(
        list, "Auth", QRCodeWifiAuthCount, qrcode_wifi_auth_change_cb, app);
    variable_item_set_current_value_index(item, app->wifi_auth);
    variable_item_set_current_value_text(item, wifi_auth_labels[app->wifi_auth]);

    variable_item_list_add(list, "Generate", 1, NULL, app);

    variable_item_list_set_enter_callback(list, qrcode_wifi_enter_cb, app);
    variable_item_list_set_selected_item(
        list, scene_manager_get_scene_state(app->scene_manager, QRCodeSceneWifi));

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewVarItemList);
}

bool qrcode_scene_wifi_on_event(void* context, SceneManagerEvent event) {
    QRCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case QRCodeWifiIndexSsid:
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneWifi, QRCodeWifiIndexSsid);
            app->edit_field = QRCodeFieldSsid;
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFieldInput);
            consumed = true;
            break;
        case QRCodeWifiIndexPass:
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneWifi, QRCodeWifiIndexPass);
            app->edit_field = QRCodeFieldPass;
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFieldInput);
            consumed = true;
            break;
        case QRCodeWifiIndexGenerate: {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneWifi, QRCodeWifiIndexGenerate);
            /* Bounded so the fully-formatted WIFI: string can never exceed
             * payload[256]: 15 literals + 6 (token) + esc_ssid + esc_pass. With
             * 118+118 the worst-case output is 255 chars, so no truncation and
             * no -Wformat-truncation. Pathological all-special-char fields (rare)
             * truncate safely inside qrcode_wifi_escape(). */
            char esc_ssid[118];
            char esc_pass[118];
            qrcode_wifi_escape(esc_ssid, sizeof(esc_ssid), app->ssid);
            qrcode_wifi_escape(esc_pass, sizeof(esc_pass), app->pass);

            if(app->wifi_auth == QRCodeWifiAuthOpen) {
                snprintf(
                    app->payload,
                    sizeof(app->payload),
                    "WIFI:S:%s;T:nopass;P:;;",
                    esc_ssid);
            } else {
                snprintf(
                    app->payload,
                    sizeof(app->payload),
                    "WIFI:S:%s;T:%s;P:%s;;",
                    esc_ssid,
                    qrcode_wifi_auth_token(app->wifi_auth),
                    esc_pass);
            }
            scene_manager_next_scene(app->scene_manager, QRCodeSceneDisplay);
            consumed = true;
            break;
        }
        default:
            break;
        }
    }
    return consumed;
}

void qrcode_scene_wifi_on_exit(void* context) {
    QRCodeApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
