#ifndef PUBLIC_BUILD
#include "../subghz_i.h"
#include <lib/subghz/receiver.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/protocols/genie.h>
#include "../helpers/genie_seq.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>

// Genie Intellicode recorder: listen on the current RX frequency and append every decoded
// Genie code to its .gne table (one file per remote+button, keyed by the code's static bits).
// Genie has no algorithm, so this captures the real remote's rolling sequence in order; the
// Genie Sender (My Garage) then emits the next unused code. Press the real remote repeatedly.
// Requires Genie to be in the active decoder set (it is added to both the general and the
// automotive registries).
#define GENIE_GARAGE_PATH EXT_PATH("subghz/garage")

static void subghz_scene_genie_record_rx_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    UNUSED(receiver);
    SubGhz* subghz = context;
    if(!decoder_base || !decoder_base->protocol || !decoder_base->protocol->name) return;
    if(strcmp(decoder_base->protocol->name, GENIE_PROTOCOL_NAME) != 0) return;

    FuriString* s = furi_string_alloc();
    subghz_protocol_decoder_base_get_string(decoder_base, s);
    const char* k = strstr(furi_string_get_cstr(s), "Key:");
    if(k) {
        unsigned long hi = 0, lo = 0;
        if(sscanf(k + 4, "%08lX%08lX", &hi, &lo) == 2) {
            uint64_t code = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
            genie_seq_append_code(GENIE_GARAGE_PATH, code); // seek+patch, fast
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher, SubGhzCustomEventSceneGenieRecordRx);
        }
    }
    furi_string_free(s);
}

static void subghz_scene_genie_record_redraw(SubGhz* subghz, uint32_t n) {
    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Genie Recorder");
    widget_add_string_multiline_element(
        subghz->widget,
        64,
        20,
        AlignCenter,
        AlignTop,
        FontSecondary,
        "Press the real remote\nrepeatedly to record");
    char line[32];
    snprintf(line, sizeof(line), "Captured: %lu", (unsigned long)n);
    widget_add_string_element(subghz->widget, 64, 52, AlignCenter, AlignTop, FontSecondary, line);
}

void subghz_scene_genie_record_on_enter(void* context) {
    SubGhz* subghz = context;
    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneGenieRecord, 0);
    subghz_scene_genie_record_redraw(subghz, 0);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
    subghz_txrx_set_rx_callback(subghz->txrx, subghz_scene_genie_record_rx_callback, subghz);
    subghz_txrx_rx_start(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateRx;
}

bool subghz_scene_genie_record_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneGenieRecordRx) {
            uint32_t n =
                scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneGenieRecord) + 1;
            scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneGenieRecord, n);
            subghz_scene_genie_record_redraw(subghz, n);
            return true;
        }
    }
    return false;
}

void subghz_scene_genie_record_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_set_rx_callback(subghz->txrx, NULL, subghz);
    subghz_txrx_stop(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    widget_reset(subghz->widget);
}

#endif // PUBLIC_BUILD (Genie recorder is private-only)
