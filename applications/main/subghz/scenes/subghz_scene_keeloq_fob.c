#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "lib/subghz/blocks/generic.h"
#include <lib/subghz/devices/devices.h>

#define TAG "SubGhzKeeloqFob"

// Bridges the fob View's callback events onto the scene manager.
static void subghz_scene_keeloq_fob_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

// Refresh the on-screen counter from the (just re-serialized) clone fff_data.
static void subghz_scene_keeloq_fob_refresh_cnt(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    uint32_t cnt = 0;
    flipper_format_rewind(fff);
    flipper_format_read_uint32(fff, "Cnt", &cnt, 1);
    subghz_keeloq_fob_set_sent(subghz->subghz_keeloq_fob, (uint16_t)cnt);
}

void subghz_scene_keeloq_fob_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->keeloq_clone_jump == 0) subghz->keeloq_clone_jump = 100;

    // Build the no-desync clone into fff_data from the gen_info the Clone action populated.
    bool ok = subghz_txrx_gen_keeloq_clone(
        subghz->txrx,
        subghz->gen_info->mod,
        subghz->gen_info->freq,
        subghz->gen_info->keeloq.serial,
        subghz->gen_info->keeloq.btn,
        subghz->gen_info->keeloq.cnt,
        subghz->keeloq_clone_jump,
        subghz->gen_info->keeloq.manuf);
    subghz->keeloq_clone_flow = false;
    if(!ok) {
        furi_string_set(
            subghz->error_str, "Function requires\nan SD card with\nfresh databases.");
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        return;
    }

    subghz_keeloq_fob_set_callback(
        subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
    subghz_keeloq_fob_set_state(
        subghz->subghz_keeloq_fob,
        (uint8_t)(subghz->gen_info->keeloq.btn & 0x0F),
        (uint16_t)subghz->gen_info->keeloq.cnt,
        subghz->keeloq_clone_jump);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
}

bool subghz_scene_keeloq_fob_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewKeeloqFobSend) {
            // Ignore new presses while a burst is still on air.
            if(subghz->state_notifications == SubGhzNotificationStateTx) return true;
            uint8_t nibble = subghz_keeloq_fob_selected_nibble(subghz->subghz_keeloq_fob);
            uint32_t btn = nibble;
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            flipper_format_update_uint32(fff, "Btn", &btn, 1);
            subghz_txrx_hopper_pause(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_block_generic_global.endless_tx = false; // single [N,N+1] burst (clone repeat=2)
            if(subghz_tx_start(subghz, fff)) {
                subghz->state_notifications = SubGhzNotificationStateTx;
            } else {
                subghz_txrx_rx_start(subghz->txrx);
                subghz_txrx_hopper_unpause(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
            }
            return true;
        } else if(event.event == SubGhzCustomEventViewKeeloqFobBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz->state_notifications == SubGhzNotificationStateTx) {
            if(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) {
                // still sending
                return true;
            }
            // burst finished
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_stop(subghz->txrx);
            subghz_scene_keeloq_fob_refresh_cnt(subghz);
        }
        return true;
    }
    return false;
}

void subghz_scene_keeloq_fob_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_stop(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    subghz->keeloq_clone_flow = false;
}
