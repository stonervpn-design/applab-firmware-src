#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"

// RollBack is a rolling-code replay/resync ATTACK — PRIVATE builds only. The whole
// scene body compiles to nothing in public (its ADD_SCENE + menu are also gated).
#ifndef PUBLIC_BUILD

#define TAG "SubGhzSceneRollbackReplay"

// RollBack Replay scene.
//
// Configures order + inter-frame delay, then (behind a confirm gate) re-transmits
// the captured rolling-code frames verbatim on a worker thread while a "Replaying"
// popup is shown. The captured counters are TX'd unchanged (no advance) — this is
// the rollback/resync technique, authorized-use only.

typedef enum {
    RollbackReplayItemOrder,
    RollbackReplayItemDelay,
    RollbackReplayItemStart,
} RollbackReplayItem;

static const char* const order_text[] = {"AsCaptured", "Ascending", "Descending"};
static const RollbackOrder order_value[] = {
    RollbackOrderAsCaptured,
    RollbackOrderAsc,
    RollbackOrderDesc};

static const char* const delay_text[] = {"50ms", "100ms", "250ms", "500ms", "1000ms"};
static const uint32_t delay_value[] = {50, 100, 250, 500, 1000};

// Single active replay scene; worker + selection state kept file-static.
static FuriThread* s_replay_thread = NULL;
static bool s_running = false;
static uint8_t s_order_index = 0;
static uint8_t s_delay_index = 1;

static void subghz_scene_rollback_replay_order_cb(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    s_order_index = index;
    variable_item_set_current_value_text(item, order_text[index]);
}

static void subghz_scene_rollback_replay_delay_cb(VariableItem* item) {
    uint8_t index = variable_item_get_current_value_index(item);
    s_delay_index = index;
    variable_item_set_current_value_text(item, delay_text[index]);
}

static int32_t subghz_scene_rollback_replay_worker(void* context) {
    SubGhz* subghz = context;
    subghz_rollback_replay(
        subghz, order_value[s_order_index], delay_value[s_delay_index], &subghz->rollback_abort);
    view_dispatcher_send_custom_event(
        subghz->view_dispatcher, SubGhzCustomEventSceneRollbackDone);
    return 0;
}

static void subghz_scene_rollback_replay_join(SubGhz* subghz) {
    if(s_replay_thread) {
        subghz->rollback_abort = true;
        furi_thread_join(s_replay_thread);
        furi_thread_free(s_replay_thread);
        s_replay_thread = NULL;
    }
    s_running = false;
}

static void subghz_scene_rollback_replay_enter_cb(void* context, uint32_t index) {
    SubGhz* subghz = context;
    if(index == RollbackReplayItemStart) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneRollbackUpdate);
    }
}

void subghz_scene_rollback_replay_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItemList* vil = subghz->variable_item_list;
    variable_item_list_reset(vil);

    VariableItem* item;
    item = variable_item_list_add(
        vil, "Order", COUNT_OF(order_text), subghz_scene_rollback_replay_order_cb, subghz);
    variable_item_set_current_value_index(item, s_order_index);
    variable_item_set_current_value_text(item, order_text[s_order_index]);

    item = variable_item_list_add(
        vil, "Delay", COUNT_OF(delay_text), subghz_scene_rollback_replay_delay_cb, subghz);
    variable_item_set_current_value_index(item, s_delay_index);
    variable_item_set_current_value_text(item, delay_text[s_delay_index]);

    variable_item_list_add(vil, "Start Replay", 0, NULL, subghz);

    variable_item_list_set_enter_callback(vil, subghz_scene_rollback_replay_enter_cb, subghz);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_rollback_replay_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneRollbackUpdate) {
            // Start Replay selected — confirm gate.
            DialogMessage* msg = dialog_message_alloc();
            dialog_message_set_header(msg, "Transmit", 64, 2, AlignCenter, AlignTop);
            dialog_message_set_text(
                msg,
                "Rolling-code replay.\nAuthorized use only?",
                64,
                32,
                AlignCenter,
                AlignCenter);
            dialog_message_set_buttons(msg, "Cancel", NULL, "Send");
            DialogMessageButton res = dialog_message_show(subghz->dialogs, msg);
            dialog_message_free(msg);

            if(res == DialogMessageButtonRight) {
                subghz->rollback_abort = false;
                s_running = true;
                popup_reset(subghz->popup);
                popup_set_header(subghz->popup, "Replaying...", 64, 26, AlignCenter, AlignCenter);
                popup_disable_timeout(subghz->popup);
                view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdPopup);

                s_replay_thread = furi_thread_alloc_ex(
                    "SubGhzRollbackReplay",
                    2048,
                    subghz_scene_rollback_replay_worker,
                    subghz);
                furi_thread_start(s_replay_thread);
            }
            consumed = true;
        } else if(event.event == SubGhzCustomEventSceneRollbackDone) {
            subghz_scene_rollback_replay_join(subghz);
            popup_reset(subghz->popup);
            view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
            notification_message(subghz->notifications, &sequence_reset_rgb);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_running) {
            // Abort the in-flight replay; the worker's Done event finalizes cleanup.
            subghz->rollback_abort = true;
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_running) {
            notification_message(subghz->notifications, &sequence_blink_magenta_10);
        }
    }
    return consumed;
}

void subghz_scene_rollback_replay_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_scene_rollback_replay_join(subghz);
    subghz_txrx_stop(subghz->txrx);
    popup_reset(subghz->popup);
    variable_item_list_reset(subghz->variable_item_list);
    notification_message(subghz->notifications, &sequence_reset_rgb);
}

#endif /* !PUBLIC_BUILD — RollBack is a private-only attack feature */
