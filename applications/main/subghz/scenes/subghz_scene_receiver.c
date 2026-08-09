#include "../subghz_i.h"
#ifndef PUBLIC_BUILD
#include "../helpers/tpms_watch.h"
#include <lib/subghz/protocols/tpms_generic.h>
#include <lib/flipper_format/flipper_format.h>
#endif
#include <dolphin/dolphin.h>
#include <lib/subghz/protocols/bin_raw.h>
#include "../helpers/subghz_capture_quality.h"

#define TAG "SubGhzSceneReceiver"

// Tick of the last decoder match + window, for the live capture-quality meter
// (private builds). Ticks are ~ms on this port (matches subghz_decode_dedup).
#define SQ_DECODE_WINDOW_MS 1500
static uint32_t s_last_decode_tick = 0;

const NotificationSequence subghz_sequence_rx = {
    &message_green_255,

    &message_display_backlight_on,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_50,
    NULL,
};

const NotificationSequence subghz_sequence_rx_locked = {
    &message_green_255,

    &message_display_backlight_on,

    &message_vibro_on,
    &message_note_c6,
    &message_delay_50,
    &message_sound_off,
    &message_vibro_off,

    &message_delay_500,

    &message_display_backlight_off,
    NULL,
};

static void subghz_scene_receiver_update_statusbar(void* context) {
    SubGhz* subghz = context;
    FuriString* history_stat_str = furi_string_alloc();
    if(!subghz_history_get_text_space_left(subghz->history, history_stat_str)) {
        FuriString* frequency_str = furi_string_alloc();
        FuriString* modulation_str = furi_string_alloc();

#ifdef SUBGHZ_EXT_PRESET_NAME
        if(subghz_history_get_last_index(subghz->history) > 0) {
            subghz_txrx_get_frequency_and_modulation(
                subghz->txrx, frequency_str, modulation_str, false);
        } else {
            FuriString* temp_str = furi_string_alloc();

            subghz_txrx_get_frequency_and_modulation(subghz->txrx, frequency_str, temp_str, true);
            furi_string_printf(
                modulation_str,
                "%s        Mod: %s",
                (subghz_txrx_radio_device_get(subghz->txrx) == SubGhzRadioDeviceTypeInternal) ?
                    "Int" :
                    "Ext",
                furi_string_get_cstr(temp_str));
            furi_string_free(temp_str);
        }
#else
        subghz_txrx_get_frequency_and_modulation(
            subghz->txrx, frequency_str, modulation_str, false);
#endif

        subghz_view_receiver_add_data_statusbar(
            subghz->subghz_receiver,
            furi_string_get_cstr(frequency_str),
            furi_string_get_cstr(modulation_str),
            furi_string_get_cstr(history_stat_str),
            subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF,
            READ_BIT(subghz->filter, SubGhzProtocolFlag_BinRAW) > 0);

        furi_string_free(frequency_str);
        furi_string_free(modulation_str);
    } else {
        subghz_view_receiver_add_data_statusbar(
            subghz->subghz_receiver,
            furi_string_get_cstr(history_stat_str),
            "",
            "",
            subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF,
            READ_BIT(subghz->filter, SubGhzProtocolFlag_BinRAW) > 0);
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    }
    furi_string_free(history_stat_str);

    subghz_view_receiver_set_radio_device_type(
        subghz->subghz_receiver, subghz_txrx_radio_device_get(subghz->txrx));
}

void subghz_scene_receiver_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

static void subghz_scene_add_to_history_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder_base,
    void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    s_last_decode_tick = furi_get_tick(); // a decoder matched -> the signal is decoding

    // TPMS scanner: only admit TPMS-sensor decodes to the list (drop garage remotes
    // and everything else on the band). Set only when entered via the private
    // "TPMS Spoofer" menu; the general Read receiver leaves tpms_scan_mode false.
    if(subghz->tpms_scan_mode && decoder_base->protocol->type != SubGhzProtocolTypeTpms) {
        subghz_receiver_reset(receiver);
        return;
    }

    // The check can be moved to /lib/subghz/receiver.c, but may result in false positives
    if((decoder_base->protocol->flag & subghz->ignore_filter) == 0) {
        SubGhzHistory* history = subghz->history;
        FuriString* item_name = furi_string_alloc();
        FuriString* item_time = furi_string_alloc();

        // --- dedup: score this decode and decide keep/replace/suppress ---
        const char* proto_name = decoder_base->protocol->name;
        uint16_t bit_count = 0; // no generic bit-count accessor; tier handles disambiguation
        uint16_t score = subghz_decode_dedup_score(proto_name, bit_count);
        uint32_t now = furi_get_tick();
        SubGhzDedupAction act =
            subghz_decode_dedup_decide(&subghz->dedup, proto_name, score, now);

        if(act == SubGhzDedupSuppress) {
            subghz_receiver_reset(receiver);
            furi_string_free(item_name);
            furi_string_free(item_time);
            return;
        }
        if(act == SubGhzDedupReplace && subghz->dedup.last_history_idx >= 0) {
            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);
            subghz_history_delete_item(history, subghz->dedup.last_history_idx);
            subghz_view_receiver_delete_item(
                subghz->subghz_receiver, subghz->dedup.last_history_idx);
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);
        }

        uint16_t idx = subghz_history_get_item(history);

        SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
        if(subghz->last_settings->delete_old_signals) {
            if(subghz_history_get_last_index(subghz->history) >= 54) {
                subghz->state_notifications = SubGhzNotificationStateRx;

                subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);

                subghz_history_delete_item(subghz->history, 0);
                subghz_view_receiver_delete_item(subghz->subghz_receiver, 0);
                subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);

                subghz_scene_receiver_update_statusbar(subghz);
                subghz->idx_menu_chosen =
                    subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
                idx--;
            }
        }
        if(subghz_history_add_to_history(history, decoder_base, &preset)) {
            furi_string_reset(item_name);
            furi_string_reset(item_time);

            subghz->state_notifications = SubGhzNotificationStateRxDone;

            subghz_history_get_text_item_menu(history, item_name, idx);
            subghz_history_get_time_item_menu(history, item_time, idx);
            subghz_view_receiver_add_item_to_menu(
                subghz->subghz_receiver,
                furi_string_get_cstr(item_name),
                furi_string_get_cstr(item_time),
                subghz_history_get_type_protocol(history, idx));

            subghz_scene_receiver_update_statusbar(subghz);
            if(subghz_history_get_text_space_left(subghz->history, NULL)) {
                notification_message(subghz->notifications, &sequence_error);
            }
#ifndef PUBLIC_BUILD
            // Car-tracker: if this TPMS sensor is on the watch list, the target
            // vehicle is nearby — alert (double vibro + blink).
            if(subghz->tpms_scan_mode) {
                TPMSBlockGeneric g = {0};
                FlipperFormat* raw = subghz_history_get_raw_data(subghz->history, idx);
                if(raw && tpms_block_generic_deserialize(&g, raw) == SubGhzProtocolStatusOk &&
                   tpms_watch_contains(g.id)) {
                    notification_message(subghz->notifications, &sequence_double_vibro);
                    notification_message(subghz->notifications, &sequence_blink_cyan_10);
                }
            }
#endif
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
            // record what we committed so the next in-window decode can compare
            subghz_decode_dedup_commit(&subghz->dedup, proto_name, score, now, idx);
        } else {
            // add failed (low heap / history full): drop dedup state so a later
            // Replace can never reuse a now-stale last_history_idx.
            subghz_decode_dedup_reset(&subghz->dedup);
        }
        subghz_receiver_reset(receiver);
        furi_string_free(item_name);
        furi_string_free(item_time);
    } else {
        FURI_LOG_D(TAG, "%s protocol ignored", decoder_base->protocol->name);
    }
}

void subghz_scene_receiver_on_enter(void* context) {
    SubGhz* subghz = context;
    SubGhzHistory* history = subghz->history;
    s_last_decode_tick = 0; // reset capture-quality decode tracking

    FuriString* item_name = furi_string_alloc();
    FuriString* item_time = furi_string_alloc();

    if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateIDLE) {
        if(subghz->tpms_scan_mode) {
#ifndef PUBLIC_BUILD
            tpms_watch_load(); // car-tracker: load watched sensor IDs for alerting
#endif
            // Force the standard TPMS modulation (433.92 MHz, 2-FSK ~47.6 kHz dev,
            // registered as preset "FM238") regardless of last_settings, so nearby
            // sensors decode. MUST go through set_preset_internal so the CC1101
            // register data is resolved from the setting — the raw set_preset(name,
            // NULL) leaves preset->data == NULL and crashes on rx_start.
            SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
            int fm_idx = subghz_setting_get_inx_preset_by_name(setting, "FM238");
            subghz_txrx_set_preset_internal(
                subghz->txrx,
                433920000,
                (uint8_t)(fm_idx >= 0 ? fm_idx : 0),
                subghz->last_settings->tx_power);
        } else {
            subghz_txrx_set_preset_internal(
                subghz->txrx,
                subghz->last_settings->frequency,
                subghz->last_settings->preset_index,
                subghz->last_settings->tx_power);
        }

        subghz->filter = subghz->last_settings->filter;
        subghz_txrx_receiver_set_filter(subghz->txrx, subghz->filter);
        subghz->ignore_filter = subghz->last_settings->ignore_filter;
        subghz->tx_power = subghz->last_settings->tx_power;

        subghz_history_reset(history);
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
        subghz->idx_menu_chosen = 0;
    }

    subghz_view_receiver_set_mode(subghz->subghz_receiver, SubGhzViewReceiverModeLive);

    // Load history to receiver
    subghz_view_receiver_exit(subghz->subghz_receiver);
    for(uint16_t i = 0; i < subghz_history_get_item(history); i++) {
        furi_string_reset(item_name);
        furi_string_reset(item_time);
        subghz_history_get_text_item_menu(history, item_name, i);
        subghz_history_get_time_item_menu(history, item_time, i);
        subghz_view_receiver_add_item_to_menu(
            subghz->subghz_receiver,
            furi_string_get_cstr(item_name),
            furi_string_get_cstr(item_time),
            subghz_history_get_type_protocol(history, i));
        subghz_rx_key_state_set(subghz, SubGhzRxKeyStateAddKey);
    }
    furi_string_free(item_name);
    furi_string_free(item_time);

    subghz_view_receiver_set_callback(
        subghz->subghz_receiver, subghz_scene_receiver_callback, subghz);
    subghz_txrx_set_rx_callback(subghz->txrx, subghz_scene_add_to_history_callback, subghz);
    subghz_decode_dedup_reset(&subghz->dedup);

    if(!subghz_history_get_text_space_left(subghz->history, NULL)) {
        subghz->state_notifications = SubGhzNotificationStateRx;
    }

    // Check if hopping was enabled (never hop in TPMS scan mode — stay on 433.92).
    if(subghz->last_settings->enable_hopping && !subghz->tpms_scan_mode) {
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateRunning);
    } else {
        subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
    }

    subghz_txrx_rx_start(subghz->txrx);
    subghz_view_receiver_set_idx_menu(subghz->subghz_receiver, subghz->idx_menu_chosen);

    //to use a universal decoder, we are looking for a link to it
    furi_check(
        subghz_txrx_load_decoder_by_name_protocol(subghz->txrx, SUBGHZ_PROTOCOL_BIN_RAW_NAME));

    subghz_scene_receiver_update_statusbar(subghz);

    subghz_view_receiver_set_lock(subghz->subghz_receiver, subghz_is_locked(subghz));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdReceiver);
}

bool subghz_scene_receiver_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        // Save cursor position before going to any other dialog
        subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);

        switch(event.event) {
        case SubGhzCustomEventViewReceiverBack:
            // Stop CC1101 Rx
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_stop(subghz->txrx);
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
            subghz_txrx_set_rx_callback(subghz->txrx, NULL, subghz);

            if(subghz_rx_key_state_get(subghz) == SubGhzRxKeyStateAddKey) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateExit);
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneNeedSaving);
            } else {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
                subghz_txrx_set_default_preset(subghz->txrx, subghz->last_settings->frequency);
                scene_manager_search_and_switch_to_previous_scene(
                    subghz->scene_manager, SubGhzSceneStart);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverOK:
            // Show file info, scene: receiver_info
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverInfo);
            dolphin_deed(DolphinDeedSubGhzReceiverInfo);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverDeleteItem:
            subghz->state_notifications = SubGhzNotificationStateRx;

            subghz_view_receiver_disable_draw_callback(subghz->subghz_receiver);

            subghz_history_delete_item(subghz->history, subghz->idx_menu_chosen);
            subghz_view_receiver_delete_item(
                subghz->subghz_receiver,
                subghz_view_receiver_get_idx_menu(subghz->subghz_receiver));
            subghz_view_receiver_enable_draw_callback(subghz->subghz_receiver);

            subghz_scene_receiver_update_statusbar(subghz);
            subghz->idx_menu_chosen = subghz_view_receiver_get_idx_menu(subghz->subghz_receiver);
            if(subghz_history_get_last_index(subghz->history) == 0) {
                subghz_rx_key_state_set(subghz, SubGhzRxKeyStateStart);
            }
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverConfig:
            // Actually signals are received but SubGhzNotificationStateRx is not working inside Config Scene
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzViewIdReceiver, SubGhzCustomEventManagerSet);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiverConfig);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverOffDisplay:
            notification_message(subghz->notifications, &sequence_display_backlight_off);
            consumed = true;
            break;
        case SubGhzCustomEventViewReceiverUnlock:
            subghz_unlock(subghz);
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF) {
            subghz_txrx_hopper_update(subghz->txrx, subghz->last_settings->hopping_threshold);
            subghz_scene_receiver_update_statusbar(subghz);
        }

        SubGhzThresholdRssiData ret_rssi = subghz_threshold_get_rssi_data(
            subghz->threshold_rssi, subghz_txrx_radio_device_get_rssi(subghz->txrx));

        subghz_receiver_rssi(subghz->subghz_receiver, ret_rssi.rssi);
        subghz_protocol_decoder_bin_raw_data_input_rssi(
            (SubGhzProtocolDecoderBinRAW*)subghz_txrx_get_decoder(subghz->txrx), ret_rssi.rssi);

#ifndef PUBLIC_BUILD
        // Live capture-quality: strong signal + no recent decode => wrong preset, etc.
        {
            float thr = subghz_threshold_rssi_get(subghz->threshold_rssi);
            bool decoded_recently = s_last_decode_tick != 0 &&
                                    (furi_get_tick() - s_last_decode_tick) < SQ_DECODE_WINDOW_MS;
            SubGhzCaptureQuality q =
                subghz_capture_quality_eval(ret_rssi.rssi, thr, decoded_recently);
            subghz_view_receiver_set_quality(subghz->subghz_receiver, q.level);
        }
#endif

        switch(subghz->state_notifications) {
        case SubGhzNotificationStateRx:
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
            break;
        case SubGhzNotificationStateRxDone:
            if(!subghz_is_locked(subghz)) {
                notification_message(subghz->notifications, &subghz_sequence_rx);
            } else {
                notification_message(subghz->notifications, &subghz_sequence_rx_locked);
            }
            subghz->state_notifications = SubGhzNotificationStateRx;
            break;
        default:
            break;
        }
    }
    return consumed;
}

void subghz_scene_receiver_on_exit(void* context) {
    SubGhz* subghz = context;
    // Leaving the receiver clears TPMS scan mode so the next normal "Read" tunes
    // from last_settings again (not forced to 433.92 FSK / TPMS-only).
    subghz->tpms_scan_mode = false;
}
