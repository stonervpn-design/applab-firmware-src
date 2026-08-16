#include "../nfc_app_i.h"

#ifndef PUBLIC_BUILD
// DESFire "Unlock / Key Attack" hub (Task 9, PRIVATE-only). Offers:
//   - Dictionary attack: sweep mf_desfire_dict_builtin() against every
//     discovered AID's key slots (NfcSceneMfDesfireDictAttack).
//   - Config audit: PASSIVE report over the already-read card data via
//     mf_desfire_audit_file / mf_desfire_audit_key_settings — no card
//     interaction, shown in-scene via the Widget view.
//   - Enter key manually: capture a 16-byte AES-128 key via ByteInput
//     in-scene, then feed it to the dict-attack scene as a 1-entry
//     "dictionary" (manual_mode) instead of the built-in one.

enum SubmenuIndex {
    SubmenuIndexDesfireUnlockDict,
    SubmenuIndexDesfireUnlockAudit,
    SubmenuIndexDesfireUnlockManual,
};

typedef enum {
    NfcSceneMfDesfireUnlockMenuStateMenu,
    NfcSceneMfDesfireUnlockMenuStateAudit,
    NfcSceneMfDesfireUnlockMenuStateManualKey,
} NfcSceneMfDesfireUnlockMenuState;

static void nfc_scene_mf_desfire_unlock_menu_submenu_callback(void* context, uint32_t index) {
    NfcApp* nfc = context;
    view_dispatcher_send_custom_event(nfc->view_dispatcher, index);
}

static void nfc_scene_mf_desfire_unlock_menu_byte_input_callback(void* context) {
    NfcApp* nfc = context;
    view_dispatcher_send_custom_event(nfc->view_dispatcher, NfcCustomEventByteInputDone);
}

static void nfc_scene_mf_desfire_unlock_menu_show_manual_key(NfcApp* nfc) {
    byte_input_set_header_text(nfc->byte_input, "Enter AES-128 key (hex)");
    byte_input_set_result_callback(
        nfc->byte_input,
        nfc_scene_mf_desfire_unlock_menu_byte_input_callback,
        NULL,
        nfc,
        nfc->mf_desfire_attack.manual_key,
        16);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewByteInput);
}

// PASSIVE config-weakness audit over the already-read card data. Never
// touches the card — mf_desfire_audit_* only formats findings from data the
// normal Read flow (or a prior dict-attack sweep) already collected.
static void nfc_scene_mf_desfire_unlock_menu_run_audit(NfcApp* nfc) {
    const MfDesfireData* data = nfc_device_get_data(nfc->nfc_device, NfcProtocolMfDesfire);

    // If a dict-attack sweep already ran this session and recovered the PICC
    // master key (AID token 0, key_no 0), reflect that in the audit instead
    // of always reporting "unverified".
    bool default_master_found = false;
    for(size_t i = 0; i < nfc->mf_desfire_attack.found_count; i++) {
        if(nfc->mf_desfire_attack.found[i].aid == 0 && nfc->mf_desfire_attack.found[i].key_no == 0) {
            default_master_found = true;
            break;
        }
    }

    furi_string_reset(nfc->text_box_store);
    furi_string_cat_printf(nfc->text_box_store, "PICC Master:\n");
    mf_desfire_audit_key_settings(
        &data->master_key_settings, default_master_found, nfc->text_box_store);

    uint32_t app_total = simple_array_get_count(data->application_ids);
    for(uint32_t i = 0; i < app_total; i++) {
        const MfDesfireApplicationId* app_id = simple_array_cget(data->application_ids, i);
        const MfDesfireApplication* app = simple_array_cget(data->applications, i);
        if(!app) continue;

        furi_string_cat_printf(
            nfc->text_box_store,
            "\nApp %02x%02x%02x:\n",
            app_id->data[2],
            app_id->data[1],
            app_id->data[0]);
        mf_desfire_audit_key_settings(&app->key_settings, false, nfc->text_box_store);

        uint32_t file_total = simple_array_get_count(app->file_ids);
        for(uint32_t f = 0; f < file_total; f++) {
            const MfDesfireFileSettings* fs = simple_array_cget(app->file_settings, f);
            mf_desfire_audit_file(fs, nfc->text_box_store);
        }
    }

    widget_reset(nfc->widget);
    widget_add_text_scroll_element(
        nfc->widget, 0, 0, 128, 64, furi_string_get_cstr(nfc->text_box_store));
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewWidget);
}

void nfc_scene_mf_desfire_unlock_menu_on_enter(void* context) {
    NfcApp* nfc = context;
    Submenu* submenu = nfc->submenu;

    submenu_add_item(
        submenu,
        "Dictionary Attack",
        SubmenuIndexDesfireUnlockDict,
        nfc_scene_mf_desfire_unlock_menu_submenu_callback,
        nfc);
    submenu_add_item(
        submenu,
        "Config Audit",
        SubmenuIndexDesfireUnlockAudit,
        nfc_scene_mf_desfire_unlock_menu_submenu_callback,
        nfc);
    submenu_add_item(
        submenu,
        "Enter Key Manually",
        SubmenuIndexDesfireUnlockManual,
        nfc_scene_mf_desfire_unlock_menu_submenu_callback,
        nfc);

    scene_manager_set_scene_state(
        nfc->scene_manager, NfcSceneMfDesfireUnlockMenu, NfcSceneMfDesfireUnlockMenuStateMenu);
    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewMenu);
}

bool nfc_scene_mf_desfire_unlock_menu_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    uint32_t state =
        scene_manager_get_scene_state(nfc->scene_manager, NfcSceneMfDesfireUnlockMenu);

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == NfcSceneMfDesfireUnlockMenuStateMenu) {
            if(event.event == SubmenuIndexDesfireUnlockDict) {
                nfc->mf_desfire_attack.manual_mode = false;
                scene_manager_next_scene(nfc->scene_manager, NfcSceneMfDesfireDictAttack);
                consumed = true;
            } else if(event.event == SubmenuIndexDesfireUnlockAudit) {
                nfc_scene_mf_desfire_unlock_menu_run_audit(nfc);
                scene_manager_set_scene_state(
                    nfc->scene_manager,
                    NfcSceneMfDesfireUnlockMenu,
                    NfcSceneMfDesfireUnlockMenuStateAudit);
                consumed = true;
            } else if(event.event == SubmenuIndexDesfireUnlockManual) {
                nfc_scene_mf_desfire_unlock_menu_show_manual_key(nfc);
                scene_manager_set_scene_state(
                    nfc->scene_manager,
                    NfcSceneMfDesfireUnlockMenu,
                    NfcSceneMfDesfireUnlockMenuStateManualKey);
                consumed = true;
            }
        } else if(state == NfcSceneMfDesfireUnlockMenuStateManualKey) {
            if(event.event == NfcCustomEventByteInputDone) {
                nfc->mf_desfire_attack.manual_mode = true;
                nfc->mf_desfire_attack.manual_key_type = MfDesfireKeyTypeAes;
                scene_manager_next_scene(nfc->scene_manager, NfcSceneMfDesfireDictAttack);
                consumed = true;
            }
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(state != NfcSceneMfDesfireUnlockMenuStateMenu) {
            widget_reset(nfc->widget);
            byte_input_set_result_callback(nfc->byte_input, NULL, NULL, NULL, NULL, 0);
            byte_input_set_header_text(nfc->byte_input, "");
            view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewMenu);
            scene_manager_set_scene_state(
                nfc->scene_manager,
                NfcSceneMfDesfireUnlockMenu,
                NfcSceneMfDesfireUnlockMenuStateMenu);
            consumed = true;
        }
        // else: unconsumed -> scene_manager pops to the previous scene.
    }

    return consumed;
}

void nfc_scene_mf_desfire_unlock_menu_on_exit(void* context) {
    NfcApp* nfc = context;

    submenu_reset(nfc->submenu);
    widget_reset(nfc->widget);
    byte_input_set_result_callback(nfc->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(nfc->byte_input, "");
    furi_string_reset(nfc->text_box_store);
}

#endif /* !PUBLIC_BUILD — DESFire unlock/key-attack hub is PRIVATE-only */
