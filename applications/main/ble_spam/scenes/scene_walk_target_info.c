#include "../ble_spam_app.h"

// PRIVATE-only: BLE GATT Fuzzer + connection-flood DoS target-info panel. The
// BleWalkFuzz/BleFuzzView types and the fuzz_* app-struct fields this file
// reads/writes only exist under #ifndef PUBLIC_BUILD (ble_spam_app.h), and the
// ADD_SCENE registration that would reference these functions is guarded the
// same way (scenes/scene_config.h) -- so on a PUBLIC_BUILD compile this file
// must compile to an empty translation unit rather than fail on the
// now-missing types/fields.
#ifndef PUBLIC_BUILD

#include "../ble_walk_hal.h"
#include "../ble_adv_decode.h"

// Full target-info panel, reached from WalkScan (Right, on the highlighted
// device -- no connection required, DoS never needs one) or re-entered after
// WalkFuzzConfig/WalkFuzzRun on Back. Renders address/name/rssi + the
// ble_adv_decode() breakdown of the device's advertising data via a
// text-scroll Widget element (mirrors subghz_scene_vuln_list.c /
// camera_hunter_scene_authorize.c's widget_add_text_scroll_element idiom),
// plus a single "Arm DoS" button that stages BleFuzzModeDos and hands off to
// the shared Fuzz/DoS config scene.
//
// Sweep/Stack are armed the other way (WalkCharDetail's "Fuzz" entry, on an
// already-connected device+characteristic) -- this scene only offers DoS,
// since DoS is the one mode that operates on a bare scan-list entry with no
// prior connect.

static void target_info_button_callback(GuiButtonType result, InputType type, void* context) {
    BleSpamApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeCenter) {
        scene_manager_handle_custom_event(app->scene_manager, BleSpamCustomEventWalkTargetInfoArmDos);
    }
}

void ble_spam_scene_walk_target_info_on_enter(void* context) {
    BleSpamApp* app = context;

    // Refresh app->fuzz_target from the live device list by the index
    // WalkScan staged in app->walk_selected_device -- covers both the normal
    // entry path and any re-entry (e.g. Back from WalkFuzzConfig then forward
    // again) where the underlying scan-list slot may have moved/refreshed.
    // If the index is stale (list shrank), fall back to whatever is already
    // staged in app->fuzz_target rather than showing a zeroed device.
    uint16_t count;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);
    if(app->walk_selected_device < count) {
        app->fuzz_target = devices[app->walk_selected_device];
    }

    BleAdvInfo adv = {0};
    ble_adv_decode(app->fuzz_target.adv_data, app->fuzz_target.adv_data_len, &adv);

    FuriString* body = furi_string_alloc();
    const uint8_t* a = app->fuzz_target.addr;
    furi_string_cat_printf(
        body,
        "\e#%s\e#\n"
        "%02X:%02X:%02X:%02X:%02X:%02X (t%d)\n"
        "RSSI: %d dBm\n",
        app->fuzz_target.name[0] ? app->fuzz_target.name : "(unknown)",
        a[0],
        a[1],
        a[2],
        a[3],
        a[4],
        a[5],
        (int)app->fuzz_target.addr_type,
        (int)app->fuzz_target.rssi);

    if(adv.has_flags) {
        furi_string_cat_printf(body, "Flags: 0x%02X\n", adv.flags);
    }
    if(adv.uuid16_count > 0) {
        furi_string_cat(body, "UUID16:");
        for(uint8_t i = 0; i < adv.uuid16_count; i++) {
            furi_string_cat_printf(body, " %04X", adv.uuid16[i]);
        }
        furi_string_cat(body, "\n");
    }
    if(adv.has_uuid128) {
        furi_string_cat(body, "UUID128:");
        for(uint8_t i = 0; i < 16; i++) {
            furi_string_cat_printf(body, " %02X", adv.uuid128[i]);
        }
        furi_string_cat(body, "\n");
    }
    if(adv.has_tx_power) {
        furi_string_cat_printf(body, "TxPower: %d dBm\n", (int)adv.tx_power);
    }
    if(adv.mfr_present) {
        furi_string_cat_printf(body, "MfrID: 0x%04X\n", adv.mfr_id);
        if(adv.mfr_data_len > 0) {
            furi_string_cat(body, "MfrData:");
            for(uint8_t i = 0; i < adv.mfr_data_len; i++) {
                furi_string_cat_printf(body, " %02X", adv.mfr_data[i]);
            }
            furi_string_cat(body, "\n");
        }
    }

    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 52, furi_string_get_cstr(body));
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Arm DoS", target_info_button_callback, app);

    // widget_element_text_scroll_create copies the text, so it's safe to free
    // the FuriString immediately (same note as subghz_scene_vuln_list.c).
    furi_string_free(body);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWalkTargetInfo);
}

bool ble_spam_scene_walk_target_info_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom &&
       event.event == BleSpamCustomEventWalkTargetInfoArmDos) {
        app->fuzz_mode = BleFuzzModeDos;
        app->fuzz_char_handle = 0; // unused by DoS
        scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkFuzzConfig);
        consumed = true;
    }

    return consumed;
}

void ble_spam_scene_walk_target_info_on_exit(void* context) {
    BleSpamApp* app = context;
    widget_reset(app->widget);
}

#endif // PUBLIC_BUILD
