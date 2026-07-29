#include "../qrcode_app_i.h"

#include <furi.h>

enum QRCodeVcardIndex {
    QRCodeVcardIndexName,
    QRCodeVcardIndexPhone,
    QRCodeVcardIndexEmail,
    QRCodeVcardIndexGenerate,
};

static void qrcode_vcard_enter_cb(void* context, uint32_t index) {
    QRCodeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void qrcode_scene_vcard_on_enter(void* context) {
    QRCodeApp* app = context;
    VariableItemList* list = app->var_item_list;
    VariableItem* item;

    variable_item_list_reset(list);
    variable_item_list_set_header(list, "Contact (vCard)");

    item = variable_item_list_add(list, "Name", 1, NULL, app);
    variable_item_set_current_value_text(item, app->name[0] ? app->name : "(empty)");

    item = variable_item_list_add(list, "Phone", 1, NULL, app);
    variable_item_set_current_value_text(item, app->phone[0] ? app->phone : "(empty)");

    item = variable_item_list_add(list, "Email", 1, NULL, app);
    variable_item_set_current_value_text(item, app->email[0] ? app->email : "(empty)");

    variable_item_list_add(list, "Generate", 1, NULL, app);

    variable_item_list_set_enter_callback(list, qrcode_vcard_enter_cb, app);
    variable_item_list_set_selected_item(
        list, scene_manager_get_scene_state(app->scene_manager, QRCodeSceneVcard));

    view_dispatcher_switch_to_view(app->view_dispatcher, QRCodeViewVarItemList);
}

bool qrcode_scene_vcard_on_event(void* context, SceneManagerEvent event) {
    QRCodeApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case QRCodeVcardIndexName:
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneVcard, QRCodeVcardIndexName);
            app->edit_field = QRCodeFieldName;
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFieldInput);
            consumed = true;
            break;
        case QRCodeVcardIndexPhone:
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneVcard, QRCodeVcardIndexPhone);
            app->edit_field = QRCodeFieldPhone;
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFieldInput);
            consumed = true;
            break;
        case QRCodeVcardIndexEmail:
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneVcard, QRCodeVcardIndexEmail);
            app->edit_field = QRCodeFieldEmail;
            scene_manager_next_scene(app->scene_manager, QRCodeSceneFieldInput);
            consumed = true;
            break;
        case QRCodeVcardIndexGenerate: {
            scene_manager_set_scene_state(
                app->scene_manager, QRCodeSceneVcard, QRCodeVcardIndexGenerate);
            /* Format into an oversized temp (worst case ~320 bytes: name*2 +
             * phone + email + literals) then copy into payload[256] with safe
             * truncation. This keeps the single-shot snprintf away from
             * -Wformat-truncation while never overflowing payload. */
            char vcard[384];
            snprintf(
                vcard,
                sizeof(vcard),
                "BEGIN:VCARD\nVERSION:3.0\nN:%s\nFN:%s\nTEL:%s\nEMAIL:%s\nEND:VCARD",
                app->name,
                app->name,
                app->phone,
                app->email);
            strncpy(app->payload, vcard, sizeof(app->payload) - 1);
            app->payload[sizeof(app->payload) - 1] = '\0';
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

void qrcode_scene_vcard_on_exit(void* context) {
    QRCodeApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
