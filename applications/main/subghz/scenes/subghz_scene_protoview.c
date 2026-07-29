#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include "../helpers/protoview_ring.h"
#include "../helpers/protoview_decode.h"
#include <lib/subghz/protocols/raw.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG        "SubGhzSceneProtoview"
#define PV_DIR     EXT_PATH("subghz/protoview")
#define PV_SUB     EXT_PATH("subghz/protoview/pv_replay.sub")
#define PV_MAX_RAW 520

// ProtoView universal-decoder scene.
//
// Feeds the RAW decoder's level/duration stream into the pure protoview_ring, then
// (on Freeze) classifies + decodes it (PWM/PT2262, Manchester, PPM/Oregon) into a
// bitstring the user can edit and re-transmit via the RAW .sub send path. Capture
// and replay are for authorized RF security research only.

// File-static UI state (single active scene instance).
static bool s_frozen = false;
static uint8_t s_decoder_index = 0; // 0=PWM 1=Manchester 2=PPM
static char s_edit_bits[257];
static uint32_t s_te = 350; // short-element duration used to re-encode bits

static void subghz_scene_protoview_data_cb(const int32_t* data, uint16_t count, void* ctx) {
    UNUSED(ctx);
    for(uint16_t i = 0; i < count; i++) {
        int32_t v = data[i];
        pv_ring_push(v >= 0, (uint32_t)(v >= 0 ? v : -v));
    }
}

static void subghz_scene_protoview_view_cb(uint32_t event, void* context) {
    SubGhz* subghz = context;
    // Map view events into the app's custom-event space to avoid collisions.
    uint32_t mapped = SubGhzCustomEventViewProtoviewFreeze;
    switch((ProtoviewEvent)event) {
    case ProtoviewEvtFreeze:
        mapped = SubGhzCustomEventViewProtoviewFreeze;
        break;
    case ProtoviewEvtDecode:
        mapped = SubGhzCustomEventViewProtoviewDecode;
        break;
    case ProtoviewEvtEdit:
        mapped = SubGhzCustomEventViewProtoviewEdit;
        break;
    case ProtoviewEvtReplay:
        mapped = SubGhzCustomEventViewProtoviewReplay;
        break;
    case ProtoviewEvtSave:
        mapped = SubGhzCustomEventViewProtoviewSave;
        break;
    case ProtoviewEvtBack:
        mapped = SubGhzCustomEventViewProtoviewBack;
        break;
    }
    view_dispatcher_send_custom_event(subghz->view_dispatcher, mapped);
}

static void subghz_scene_protoview_callback_end_tx(void* context) {
    SubGhz* subghz = context;
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    subghz_txrx_stop(subghz->txrx);
}

static void subghz_scene_protoview_update_status(SubGhz* subghz) {
    FuriString* freq = furi_string_alloc();
    FuriString* mod = furi_string_alloc();
    subghz_txrx_get_frequency_and_modulation(subghz->txrx, freq, mod, false);
    char buf[24];
    snprintf(
        buf, sizeof(buf), "%s %s", furi_string_get_cstr(freq), furi_string_get_cstr(mod));
    subghz_view_protoview_set_status(subghz->subghz_protoview, buf);
    furi_string_free(freq);
    furi_string_free(mod);
}

static void subghz_scene_protoview_start_rx(SubGhz* subghz) {
    furi_check(subghz_txrx_load_decoder_by_name_protocol(subghz->txrx, SUBGHZ_PROTOCOL_RAW_NAME));
    subghz_txrx_receiver_set_filter(subghz->txrx, SubGhzProtocolFlag_RAW);
    SubGhzProtocolDecoderRAW* raw =
        (SubGhzProtocolDecoderRAW*)subghz_txrx_get_decoder(subghz->txrx);
    subghz_protocol_decoder_raw_set_data_callback(raw, subghz_scene_protoview_data_cb, subghz);
    subghz_txrx_rx_start(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateRx;
}

void subghz_scene_protoview_on_enter(void* context) {
    SubGhz* subghz = context;

    // Lazy-load: allocate + register the ProtoView view only while this scene is
    // active to keep peak heap low on this no-PSRAM board. Idempotent on re-entry.
    if(!subghz->subghz_protoview) {
        subghz->subghz_protoview = subghz_view_protoview_alloc();
        view_dispatcher_add_view(
            subghz->view_dispatcher,
            SubGhzViewIdProtoview,
            subghz_view_protoview_get_view(subghz->subghz_protoview));
    }

    s_frozen = false;
    s_decoder_index = 0;
    s_edit_bits[0] = '\0';

    subghz_txrx_set_preset_internal(
        subghz->txrx,
        subghz->last_settings->frequency,
        subghz->last_settings->preset_index,
        subghz->last_settings->tx_power);

    // Allocate the capture ring only for the lifetime of this scene (freed in
    // on_exit). If the heap can't spare it right now, ProtoView still opens but
    // shows no captured signal rather than crashing.
    pv_ring_alloc();
    pv_ring_reset();
    pv_ring_freeze(false);

    subghz_view_protoview_set_callback(
        subghz->subghz_protoview, subghz_scene_protoview_view_cb, subghz);
    subghz_view_protoview_set_frozen(subghz->subghz_protoview, false);
    subghz_view_protoview_set_bits(subghz->subghz_protoview, "", "");
    subghz_scene_protoview_update_status(subghz);

    subghz_scene_protoview_start_rx(subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdProtoview);
}

static void subghz_scene_protoview_do_decode(SubGhz* subghz) {
    const PvSample* w = NULL;
    size_t n = pv_ring_snapshot(&w);
    PvThresholds th = {0};
    if(!pv_classify_symbols(w, n, &th)) {
        subghz_view_protoview_set_bits(subghz->subghz_protoview, "", "no clear symbols");
        return;
    }
    s_te = th.t_short ? th.t_short : 350;

    char bits[257];
    int nb = 0;
    const char* label = "";
    s_decoder_index = (uint8_t)((s_decoder_index) % 3);
    if(s_decoder_index == 0) {
        nb = pv_decode_pwm(w, n, &th, bits, sizeof(bits));
        label = "PWM(PT2262)";
    } else if(s_decoder_index == 1) {
        nb = pv_decode_manchester(w, n, &th, bits, sizeof(bits));
        label = "Manchester";
    } else {
        nb = pv_decode_ppm(w, n, &th, bits, sizeof(bits));
        label = "PPM(Oregon)";
    }
    s_decoder_index = (uint8_t)((s_decoder_index + 1) % 3); // cycle for next press

    if(nb <= 0) {
        bits[0] = '\0';
        subghz_view_protoview_set_bits(subghz->subghz_protoview, "", label);
    } else {
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "%s %db", label, nb);
        subghz_view_protoview_set_bits(subghz->subghz_protoview, bits, lbl);
    }
}

static void subghz_scene_protoview_do_save(SubGhz* subghz) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, PV_DIR);
    File* file = storage_file_alloc(storage);
    FuriString* path = furi_string_alloc();
    furi_string_printf(
        path, "%s/pv_%lu.pvr", PV_DIR, (unsigned long)furi_get_tick());
    if(storage_file_open(
           file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const PvSample* w = NULL;
        size_t n = pv_ring_snapshot(&w);
        char line[24];
        for(size_t i = 0; i < n; i++) {
            int len = snprintf(
                line, sizeof(line), "%lu %u\n", (unsigned long)w[i].dur, (unsigned)w[i].level);
            storage_file_write(file, line, (size_t)len);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    notification_message(subghz->notifications, &sequence_success);
}

static void subghz_scene_protoview_do_replay(SubGhz* subghz) {
    const char* edited = subghz_view_protoview_get_bits(subghz->subghz_protoview);
    strncpy(s_edit_bits, edited, sizeof(s_edit_bits) - 1);
    s_edit_bits[sizeof(s_edit_bits) - 1] = '\0';
    if(strlen(s_edit_bits) == 0) return;

    // Heap-allocate the large working buffers (8 KB app stack is tight).
    PvRawSymbol* raw = malloc(sizeof(PvRawSymbol) * PV_MAX_RAW);
    int32_t* line = malloc(sizeof(int32_t) * 512);
    size_t nr = pv_bits_to_raw(s_edit_bits, s_te, raw, PV_MAX_RAW);

    uint32_t freq = subghz_txrx_get_preset(subghz->txrx).frequency;

    FlipperFormat* f = flipper_format_file_alloc(furi_record_open(RECORD_STORAGE));
    bool ok = flipper_format_file_open_always(f, PV_SUB);
    if(ok) ok = flipper_format_write_header_cstr(f, "Flipper SubGhz RAW File", 1);
    if(ok) ok = flipper_format_write_uint32(f, "Frequency", &freq, 1);
    if(ok)
        ok = flipper_format_write_string_cstr(
            f, "Preset", "FuriHalSubGhzPresetOok650Async");
    if(ok) ok = flipper_format_write_string_cstr(f, "Protocol", "RAW");
    size_t k = 0;
    for(size_t i = 0; ok && i < nr; i++) {
        line[k++] = raw[i].level ? (int32_t)raw[i].dur : -(int32_t)raw[i].dur;
        if(k == 512) {
            ok = flipper_format_write_int32(f, "RAW_Data", line, k);
            k = 0;
        }
    }
    if(ok && k) ok = flipper_format_write_int32(f, "RAW_Data", line, k);
    flipper_format_free(f);
    furi_record_close(RECORD_STORAGE);
    free(raw);
    free(line);

    if(!ok) {
        notification_message(subghz->notifications, &sequence_error);
        return;
    }

    // Build send format from the freshly-written RAW .sub and TX it.
    subghz_protocol_raw_gen_fff_data(
        subghz_txrx_get_fff_data(subghz->txrx),
        PV_SUB,
        subghz_txrx_radio_device_get_name(subghz->txrx));
    if(subghz_tx_start(subghz, subghz_txrx_get_fff_data(subghz->txrx))) {
        subghz_txrx_set_raw_file_encoder_worker_callback_end(
            subghz->txrx, subghz_scene_protoview_callback_end_tx, subghz);
        subghz->state_notifications = SubGhzNotificationStateTx;
    }
}

bool subghz_scene_protoview_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    SubGhzProtocolDecoderRAW* raw =
        (SubGhzProtocolDecoderRAW*)subghz_txrx_get_decoder(subghz->txrx);

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case SubGhzCustomEventViewProtoviewFreeze:
            s_frozen = !s_frozen;
            pv_ring_freeze(s_frozen);
            subghz_view_protoview_set_frozen(subghz->subghz_protoview, s_frozen);
            if(s_frozen) {
                subghz_txrx_stop(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateIDLE;
            } else {
                subghz_scene_protoview_start_rx(subghz);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewProtoviewDecode:
            subghz_scene_protoview_do_decode(subghz);
            consumed = true;
            break;
        case SubGhzCustomEventViewProtoviewEdit:
            consumed = true; // view handles mode switch internally
            break;
        case SubGhzCustomEventViewProtoviewReplay: {
            DialogMessage* msg = dialog_message_alloc();
            dialog_message_set_header(msg, "Transmit", 64, 2, AlignCenter, AlignTop);
            dialog_message_set_text(
                msg,
                "Replay edited frame.\nAuthorized use only?",
                64,
                32,
                AlignCenter,
                AlignCenter);
            dialog_message_set_buttons(msg, "Cancel", NULL, "Send");
            DialogMessageButton res = dialog_message_show(subghz->dialogs, msg);
            dialog_message_free(msg);
            if(res == DialogMessageButtonRight) {
                subghz_scene_protoview_do_replay(subghz);
            }
            consumed = true;
            break;
        }
        case SubGhzCustomEventViewProtoviewSave:
            subghz_scene_protoview_do_save(subghz);
            consumed = true;
            break;
        case SubGhzCustomEventViewProtoviewBack:
            subghz_txrx_stop(subghz->txrx);
            if(raw) subghz_protocol_decoder_raw_set_data_callback(raw, NULL, NULL);
            subghz_txrx_receiver_set_filter(subghz->txrx, subghz->filter);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            if(!scene_manager_search_and_switch_to_previous_scene(
                   subghz->scene_manager, SubGhzSceneStart)) {
                scene_manager_stop(subghz->scene_manager);
                view_dispatcher_stop(subghz->view_dispatcher);
            }
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        switch(subghz->state_notifications) {
        case SubGhzNotificationStateRx:
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
            break;
        case SubGhzNotificationStateTx:
            notification_message(subghz->notifications, &sequence_blink_magenta_10);
            break;
        default:
            break;
        }
    }
    return consumed;
}

void subghz_scene_protoview_on_exit(void* context) {
    SubGhz* subghz = context;
    SubGhzProtocolDecoderRAW* raw =
        (SubGhzProtocolDecoderRAW*)subghz_txrx_get_decoder(subghz->txrx);
    subghz_txrx_stop(subghz->txrx);
    if(raw) subghz_protocol_decoder_raw_set_data_callback(raw, NULL, NULL);
    subghz_txrx_receiver_set_filter(subghz->txrx, subghz->filter);
    pv_ring_freeze(false);
    // Worker is stopped and the data callback nulled above, so no pv_ring_push
    // can be in flight — safe to release the 16 KB capture buffer back to heap.
    pv_ring_free();
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    notification_message(subghz->notifications, &sequence_reset_rgb);

    // Lazy-load: free + unregister the ProtoView view now the scene is leaving.
    if(subghz->subghz_protoview) {
        view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdProtoview);
        subghz_view_protoview_free(subghz->subghz_protoview);
        subghz->subghz_protoview = NULL;
    }
}
