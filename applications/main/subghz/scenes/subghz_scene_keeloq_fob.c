#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include "lib/subghz/blocks/generic.h"
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/protocols/vag.h>

#define TAG "SubGhzKeeloqFob"

// Bridges the fob View's callback events onto the scene manager.
static void subghz_scene_keeloq_fob_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

// Refresh the on-screen counter from fff_data after TX. Both the KeeLoq clone and the
// VAG encoder advance AND persist the counter into fff_data during subghz_tx_start, so
// we only read the post-TX value here — no manual +1 (that would double-advance VAG).
static void subghz_scene_keeloq_fob_refresh_cnt(SubGhz* subghz) {
    FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
    uint32_t cnt = 0;
    flipper_format_rewind(fff);
    flipper_format_read_uint32(fff, "Cnt", &cnt, 1);
    subghz_keeloq_fob_set_sent(subghz->subghz_keeloq_fob, cnt);
}

void subghz_scene_keeloq_fob_on_enter(void* context) {
    SubGhz* subghz = context;
    // Desync-safe default: NO forward lead (jump stays 0). A clone advances the shared
    // rolling counter by +1 like the real fob, so the original never gets stranded — its
    // worst-case catch-up is only as many presses as the clone was used. (Was 100; that
    // +100 leap is exactly what desynced the original fob. The SetJump scene can still
    // opt into a small lead for reliability at the cost of a few original catch-up presses.)

    // Detect the loaded frame's protocol, but ONLY on a from_garage entry. VAG is
    // always reached via Save->Garage. A fresh KeeLoq clone flow (keeloq_clone_flow,
    // from_garage=false) reaches this scene with STALE fff_data that may still hold
    // "Protocol: VAG" from a prior VAG emulate; reading it there would mis-route the
    // KeeLoq clone into the VAG branch and skip gen_keeloq_clone. KeeLoq Garage entries
    // are also from_garage but carry a fresh "Protocol: KeeLoq", so the string check
    // keeps them on the KeeLoq path below.
    bool is_vag = false;
    if(subghz->keeloq_fob_from_garage) {
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        FuriString* proto = furi_string_alloc();
        flipper_format_rewind(fff);
        if(flipper_format_read_string(fff, "Protocol", proto) &&
           furi_string_equal_str(proto, VAG_PROTOCOL_NAME)) {
            is_vag = true;
        }
        furi_string_free(proto);
    }

    if(is_vag) {
        subghz->keeloq_fob_from_garage = false; // consume the flag
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        uint32_t btn = 0, cnt_file = 0;
        flipper_format_rewind(fff);
        flipper_format_read_uint32(fff, "Btn", &btn, 1);
        flipper_format_rewind(fff);
        flipper_format_read_uint32(fff, "Cnt", &cnt_file, 1);

        // Normalize VAG button code to the low-nibble form (0x1/0x2/0x4) the fob
        // tiles use; some captures carry the 0x10/0x20/0x40 alternate encoding.
        uint8_t rb = (uint8_t)btn;
        if(rb & 0x70) rb >>= 4;
        rb &= 0x07;

        // Desync-safe: no forward lead by default (vjump 0) — start at the captured
        // counter and let the encoder advance +1 per press, so the original recovers in
        // 1-2 presses. KeyIdx/Type stay in fff; the encoder re-encrypts from Cnt/Btn.
        uint16_t vjump = subghz->keeloq_clone_jump; // 0 = no lead
        uint32_t cnt_start = cnt_file + vjump;
        flipper_format_update_uint32(fff, "Cnt", &cnt_start, 1);

        subghz_keeloq_fob_set_callback(
            subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
        subghz_keeloq_fob_set_state(
            subghz->subghz_keeloq_fob, SubGhzFobKindVag, rb, cnt_start, vjump);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
        return;
    }

    uint8_t real_btn;
    uint16_t cnt;
    uint16_t jump;

    if(subghz->keeloq_fob_from_garage) {
        // Re-opened from the Garage: fff_data already holds the loaded CloneMode .sub.
        subghz->keeloq_fob_from_garage = false;
        FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
        uint32_t v = 0;
        flipper_format_rewind(fff);
        if(!(flipper_format_read_uint32(fff, "CloneMode", &v, 1) && v == 1)) {
            furi_string_set(subghz->error_str, "Not a saved fob\n(no CloneMode).");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
            return;
        }
        flipper_format_rewind(fff);
        v = 0;
        flipper_format_read_uint32(fff, "Btn", &v, 1);
        real_btn = (uint8_t)(v & 0x0F);
        flipper_format_rewind(fff);
        v = 0;
        flipper_format_read_uint32(fff, "Cnt", &v, 1);
        cnt = (uint16_t)v;
        flipper_format_rewind(fff);
        v = subghz->keeloq_clone_jump;
        flipper_format_read_uint32(fff, "CloneJump", &v, 1);
        jump = (uint16_t)v;
    } else {
        // Build the clone into fff_data from the gen_info the Clone action set. BFT/Erreka
        // (non-zero seed) route through the seed-aware builder so the derived device key is
        // correct; normal KeeLoq uses the no-desync clone builder.
        bool ok =
            subghz->gen_info->keeloq.seed ?
                subghz_txrx_gen_keeloq_bft_protocol(
                    subghz->txrx,
                    subghz->gen_info->mod,
                    subghz->gen_info->freq,
                    subghz->gen_info->keeloq.serial,
                    subghz->gen_info->keeloq.btn,
                    subghz->gen_info->keeloq.cnt,
                    subghz->gen_info->keeloq.seed,
                    subghz->gen_info->keeloq.manuf) :
                subghz_txrx_gen_keeloq_clone(
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
        real_btn = (uint8_t)(subghz->gen_info->keeloq.btn & 0x0F);
        cnt = (uint16_t)subghz->gen_info->keeloq.cnt;
        jump = subghz->keeloq_clone_jump;
    }

    subghz_keeloq_fob_set_callback(
        subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
    subghz_keeloq_fob_set_state(
        subghz->subghz_keeloq_fob, SubGhzFobKindKeeloq, real_btn, cnt, jump);
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
