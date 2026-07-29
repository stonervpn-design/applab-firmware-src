// WhisperPair (CVE-2025-36911) surface detector: identifies Google Fast Pair
// (0xFE2C) devices and enumerates their GATT attack surface READ-ONLY. It never
// writes the Key-Based Pairing characteristic — it only reports whether that
// surface exists (+ a known-vulnerable model-name hint). No bond, no exploit.
#include "../ble_spam_app.h"
#include "../ble_walk_hal.h"
#include "../whisperpair_fp.h"
#include "../views/whisperpair_detector_view.h"

#include <string.h>

// Fast Pair GATT characteristics (Google Fast Pair spec). ESP-IDF stores 128-bit
// UUIDs little-endian, so each array is the textual UUID reversed.
// Model ID:          FE2C1233-8366-4814-8EB0-01DE32100BEA
static const uint8_t FP_UUID_MODEL_ID[16] = {
    0xEA, 0x0B, 0x10, 0x32, 0xDE, 0x01, 0xB0, 0x8E,
    0x14, 0x48, 0x66, 0x83, 0x33, 0x12, 0x2C, 0xFE,
};
// Key-based Pairing:  FE2C1234-8366-4814-8EB0-01DE32100BEA  (the WhisperPair surface)
static const uint8_t FP_UUID_KBP[16] = {
    0xEA, 0x0B, 0x10, 0x32, 0xDE, 0x01, 0xB0, 0x8E,
    0x14, 0x48, 0x66, 0x83, 0x34, 0x12, 0x2C, 0xFE,
};

#define FP_SERVICE_UUID16 0xFE2C

static bool uuid_is_fp_service(const esp_bt_uuid_t* u) {
    if(u->len == ESP_UUID_LEN_16) return u->uuid.uuid16 == FP_SERVICE_UUID16;
    if(u->len == ESP_UUID_LEN_128) {
        // SIG base 0000FE2C-0000-1000-8000-00805F9B34FB, little-endian.
        static const uint8_t base[16] = {
            0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x2C, 0xFE, 0x00, 0x00,
        };
        return memcmp(u->uuid.uuid128, base, 16) == 0;
    }
    return false;
}

static bool uuid_is_128(const esp_bt_uuid_t* u, const uint8_t ref[16]) {
    return u->len == ESP_UUID_LEN_128 && memcmp(u->uuid.uuid128, ref, 16) == 0;
}

static void refresh_device_list(BleSpamApp* app) {
    uint16_t count = 0;
    BleWalkDevice* devices = ble_walk_hal_get_devices(&count);

    WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);

    for(uint16_t i = 0; i < count; i++) {
        BleWalkDevice* src = &devices[i];

        uint32_t model_id = 0;
        FpAdvKind k = fp_adv_parse(src->adv_data, src->adv_data_len, &model_id);
        if(k == FpAdvNone && src->scan_rsp_len) {
            k = fp_adv_parse(src->scan_rsp_data, src->scan_rsp_len, &model_id);
        }
        if(k == FpAdvNone) continue; // not a Fast Pair advertiser

        bool found = false;
        for(uint16_t j = 0; j < model->count; j++) {
            if(memcmp(model->devices[j].addr, src->addr, sizeof(esp_bd_addr_t)) == 0) {
                model->devices[j].rssi = src->rssi;
                model->devices[j].adv_kind = k;
                if(k == FpAdvDiscoverable && !model->devices[j].model_id_valid) {
                    model->devices[j].model_id = model_id;
                    model->devices[j].model_id_valid = true;
                }
                if(src->name[0]) {
                    strncpy(
                        model->devices[j].name,
                        src->name,
                        sizeof(model->devices[j].name) - 1);
                    model->devices[j].name[sizeof(model->devices[j].name) - 1] = '\0';
                }
                found = true;
                break;
            }
        }
        if(!found && model->count < WP_DETECTOR_MAX_DEVICES) {
            WpDevice* dst = &model->devices[model->count];
            memcpy(dst->addr, src->addr, sizeof(esp_bd_addr_t));
            dst->addr_type = src->addr_type;
            dst->rssi = src->rssi;
            dst->adv_kind = k;
            dst->model_id = model_id;
            dst->model_id_valid = (k == FpAdvDiscoverable);
            strncpy(dst->name, src->name, sizeof(dst->name) - 1);
            dst->name[sizeof(dst->name) - 1] = '\0';
            dst->status = WpStatusUnknown;
            model->count++;
        }
    }

    if(model->count > 0 && model->selected >= model->count) {
        model->selected = model->count - 1;
    }
    view_commit_model(app->view_whisperpair_detector, true);
}

// READ-ONLY probe: connect, confirm the 0xFE2C service + the KBP
// characteristic (the surface), best-effort READ of Model ID, disconnect.
// There is deliberately NO write to any characteristic here.
static WpStatus probe_device(BleSpamApp* app, WpDevice* target) {
    BleWalkDevice walk_dev = {0};
    memcpy(walk_dev.addr, target->addr, sizeof(esp_bd_addr_t));
    walk_dev.addr_type = target->addr_type;

    app->wp_probe_abort = false;
    if(!ble_walk_hal_connect(&walk_dev, &app->wp_probe_abort)) {
        return WpStatusConnectFail;
    }
    if(!ble_walk_hal_discover_services()) {
        ble_walk_hal_disconnect();
        return WpStatusConnectFail;
    }
    for(int i = 0; i < 60 && !ble_walk_hal_services_ready(); i++) {
        if(app->wp_probe_abort) {
            ble_walk_hal_disconnect();
            return WpStatusConnectFail;
        }
        furi_delay_ms(50);
    }
    if(!ble_walk_hal_services_ready()) {
        ble_walk_hal_disconnect();
        return WpStatusConnectFail;
    }

    uint16_t svc_count = 0;
    BleWalkService* services = ble_walk_hal_get_services(&svc_count);
    BleWalkService* fp = NULL;
    for(uint16_t i = 0; i < svc_count; i++) {
        if(uuid_is_fp_service(&services[i].uuid)) {
            fp = &services[i];
            break;
        }
    }
    if(!fp) {
        ble_walk_hal_disconnect();
        return WpStatusNoSurface;
    }

    bool kbp_present = false;
    uint16_t model_handle = 0;
    if(ble_walk_hal_discover_chars(fp)) {
        for(int i = 0; i < 60 && !ble_walk_hal_chars_ready(); i++) {
            if(app->wp_probe_abort) {
                ble_walk_hal_disconnect();
                return WpStatusConnectFail;
            }
            furi_delay_ms(50);
        }
        if(ble_walk_hal_chars_ready()) {
            uint16_t ch_count = 0;
            BleWalkChar* chars = ble_walk_hal_get_chars(&ch_count);
            for(uint16_t i = 0; i < ch_count; i++) {
                if(uuid_is_128(&chars[i].uuid, FP_UUID_KBP)) kbp_present = true;
                if(uuid_is_128(&chars[i].uuid, FP_UUID_MODEL_ID))
                    model_handle = chars[i].handle;
            }
        }
    }

    // Best-effort READ of Model ID for display (never changes the verdict).
    if(kbp_present && model_handle && ble_walk_hal_read_char(model_handle)) {
        for(int i = 0; i < 40 && !ble_walk_hal_read_ready(); i++) {
            if(app->wp_probe_abort) break;
            furi_delay_ms(50);
        }
        if(ble_walk_hal_read_ready()) {
            uint16_t vlen = 0;
            uint8_t* v = ble_walk_hal_get_read_value(&vlen);
            if(v && vlen >= 3) {
                target->model_id =
                    ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | (uint32_t)v[2];
                target->model_id_valid = true;
            }
        }
    }

    ble_walk_hal_disconnect();

    if(!kbp_present) return WpStatusNoSurface;
    return fp_name_is_vuln(target->name) ? WpStatusLikelyVuln : WpStatusSurface;
}

void ble_spam_scene_whisperpair_detector_on_enter(void* context) {
    BleSpamApp* app = context;

    WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);
    memset(model, 0, sizeof(WpDetectorModel));
    view_commit_model(app->view_whisperpair_detector, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewWhisperpairDetector);

    if(!ble_walk_hal_start()) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    ble_walk_hal_start_scan();
}

bool ble_spam_scene_whisperpair_detector_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);

        if(event.event == InputKeyUp) {
            if(model->selected > 0) {
                model->selected--;
                if(model->selected < model->window_offset)
                    model->window_offset = model->selected;
            }
            view_commit_model(app->view_whisperpair_detector, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(model->count > 0 && model->selected < model->count - 1) {
                model->selected++;
                if(model->selected >= model->window_offset + WP_DETECTOR_ITEMS_ON_SCREEN)
                    model->window_offset =
                        model->selected - WP_DETECTOR_ITEMS_ON_SCREEN + 1;
            }
            view_commit_model(app->view_whisperpair_detector, true);
            consumed = true;
        } else if(event.event == InputKeyOk) {
            if(model->count == 0 || model->selected >= model->count) {
                view_commit_model(app->view_whisperpair_detector, false);
                return true;
            }

            WpDevice target_copy = model->devices[model->selected];
            uint16_t selected_idx = model->selected;

            model->devices[selected_idx].status = WpStatusProbing;
            model->probing = true;
            view_commit_model(app->view_whisperpair_detector, true);

            ble_walk_hal_stop_scan();

            WpStatus result = probe_device(app, &target_copy);

            WpDetectorModel* m2 = view_get_model(app->view_whisperpair_detector);
            for(uint16_t i = 0; i < m2->count; i++) {
                if(memcmp(m2->devices[i].addr, target_copy.addr, sizeof(esp_bd_addr_t)) ==
                   0) {
                    m2->devices[i].status = result;
                    m2->devices[i].model_id = target_copy.model_id;
                    m2->devices[i].model_id_valid = target_copy.model_id_valid;
                    break;
                }
            }
            m2->probing = false;
            view_commit_model(app->view_whisperpair_detector, true);

            ble_walk_hal_start_scan();
            consumed = true;
        } else {
            // Release the locking view model for any other custom event
            // (unreachable today — the view only emits Ok/Up/Down — but avoids
            // holding the model mutex if a new custom event is ever added).
            view_commit_model(app->view_whisperpair_detector, false);
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        app->wp_probe_abort = true;
        ble_walk_hal_disconnect();
        ble_walk_hal_stop_scan();
        ble_walk_hal_stop();
        consumed = false;
    } else if(event.type == SceneManagerEventTypeTick) {
        WpDetectorModel* model = view_get_model(app->view_whisperpair_detector);
        if(!model->probing) {
            view_commit_model(app->view_whisperpair_detector, false);
            refresh_device_list(app);
        } else {
            view_commit_model(app->view_whisperpair_detector, false);
        }
    }

    return consumed;
}

void ble_spam_scene_whisperpair_detector_on_exit(void* context) {
    BleSpamApp* app = context;
    app->wp_probe_abort = true;
    ble_walk_hal_disconnect();
    ble_walk_hal_stop_scan();
    ble_walk_hal_stop();
}
