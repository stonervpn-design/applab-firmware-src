#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "lib/subghz/blocks/generic.h"
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/protocols/genie.h>
#include "../helpers/genie_seq.h"
#include <stdlib.h>

// Genie Intellicode "next-code" sender. Genie has no algorithm; this transmits the next
// UNUSED code from a recorded .gne table (see genie_seq / subghz_scene_genie_record). Reached
// from My Garage when a .gne is selected (subghz->file_path). ARF-recorded .gne tables work here
// too. Genie is OOK; frequency is 315 or 390 MHz — we use the app's current tuned frequency.
#define GENIE_SEND_PRESET  "AM650"
#define GENIE_DEFAULT_FREQ 390000000UL

static void subghz_scene_genie_send_callback(GuiButtonType result, InputType type, void* context) {
    SubGhz* subghz = context;
    if(result == GuiButtonTypeCenter && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneGenieSend);
    } else if(result == GuiButtonTypeRight && type == InputTypeShort) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneGenieReset);
    }
}

static uint32_t subghz_scene_genie_send_freq(SubGhz* subghz) {
    FuriString* f = furi_string_alloc();
    FuriString* m = furi_string_alloc();
    subghz_txrx_get_frequency_and_modulation(subghz->txrx, f, m, false);
    double mhz = strtod(furi_string_get_cstr(f), NULL);
    furi_string_free(f);
    furi_string_free(m);
    uint32_t hz = (uint32_t)(mhz * 1000000.0 + 0.5);
    return (hz >= 300000000UL && hz <= 928000000UL) ? hz : GENIE_DEFAULT_FREQ;
}

static void subghz_scene_genie_send_redraw(SubGhz* subghz) {
    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Genie Sender");
    uint32_t sn = 0;
    uint16_t last = 0, count = 0;
    const char* path = furi_string_get_cstr(subghz->file_path);
    if(!genie_seq_stat_path(path, &sn, &last, &count)) {
        widget_add_string_multiline_element(
            subghz->widget, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Not a Genie table");
        return;
    }
    char line[48];
    snprintf(line, sizeof(line), "Remote %08lX", (unsigned long)sn);
    widget_add_string_element(subghz->widget, 64, 18, AlignCenter, AlignTop, FontSecondary, line);
    if(last < count) {
        snprintf(line, sizeof(line), "Next: %u of %u", (unsigned)(last + 1), (unsigned)count);
    } else {
        snprintf(line, sizeof(line), "Exhausted (%u codes)", (unsigned)count);
    }
    widget_add_string_element(subghz->widget, 64, 32, AlignCenter, AlignTop, FontSecondary, line);
    if(last < count) {
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeCenter,
            "Send",
            subghz_scene_genie_send_callback,
            subghz);
    }
    widget_add_button_element(
        subghz->widget, GuiButtonTypeRight, "Reset", subghz_scene_genie_send_callback, subghz);
}

void subghz_scene_genie_send_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_scene_genie_send_redraw(subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_genie_send_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    const char* path = furi_string_get_cstr(subghz->file_path);
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneGenieSend) {
            if(subghz->state_notifications == SubGhzNotificationStateTx) return true;
            uint32_t sn = 0;
            uint16_t last = 0, count = 0;
            if(!genie_seq_stat_path(path, &sn, &last, &count) || last >= count) return true;
            uint64_t code = genie_seq_code_at_path(path, last);
            if(!subghz_txrx_gen_data_protocol(
                   subghz->txrx,
                   GENIE_SEND_PRESET,
                   subghz_scene_genie_send_freq(subghz),
                   GENIE_PROTOCOL_NAME,
                   code,
                   64)) {
                return true;
            }
            subghz_txrx_hopper_pause(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_block_generic_global.endless_tx = false;
            if(subghz_tx_start(subghz, subghz_txrx_get_fff_data(subghz->txrx))) {
                subghz->state_notifications = SubGhzNotificationStateTx;
                genie_seq_set_last_sent_path(path, (uint16_t)(last + 1)); // advance cursor
            } else {
                subghz_txrx_rx_start(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
            }
            subghz_scene_genie_send_redraw(subghz);
            return true;
        } else if(event.event == SubGhzCustomEventSceneGenieReset) {
            genie_seq_set_last_sent_path(path, 0);
            subghz_scene_genie_send_redraw(subghz);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz->state_notifications == SubGhzNotificationStateTx) {
            if(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) return true;
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_stop(subghz->txrx);
        }
        return true;
    }
    return false;
}

void subghz_scene_genie_send_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_stop(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    widget_reset(subghz->widget);
}
