#include "../nfc_app_i.h"

#include <furi.h>

/*
 * On-device mfkey32 key recovery.
 *
 * Replaces the previous "run external mfkey.fap" offload. When the user chooses
 * "Recover", a FuriThread worker iterates every collected nonce param set and
 * runs the (heavy: ~112 KB heap, up to ~1 min, recursive) crapto1 recovery on
 * each. The worker NEVER touches the GUI: it publishes progress via
 * view_dispatcher_send_custom_event() (thread-safe) and the GUI thread updates
 * the popup/widget from the shared NfcMfkeyRecoverContext. Back during recovery
 * sets a cancel flag that the worker + crapto1 cancel callback poll; the thread
 * is always joined + freed before the scene leaves (on WorkerExit or on_exit).
 */

typedef enum {
    MfkeyCompleteStatePrompt, // menu: Recover / Finish
    MfkeyCompleteStateRecovering, // progress popup, worker running
    MfkeyCompleteStateResults, // recovered-keys summary
    MfkeyCompleteStateSaved, // "added to dict" summary
} NfcSceneMfClassicMfKeyCompleteState;

static NfcSceneMfClassicMfKeyCompleteState nfc_scene_mfkey_complete_get_state(NfcApp* instance) {
    return scene_manager_get_scene_state(instance->scene_manager, NfcSceneMfClassicMfkeyComplete);
}

static void
    nfc_scene_mfkey_complete_set_state(NfcApp* instance, NfcSceneMfClassicMfKeyCompleteState state) {
    scene_manager_set_scene_state(instance->scene_manager, NfcSceneMfClassicMfkeyComplete, state);
}

/* ---------- worker thread ---------- */

static bool nfc_scene_mfkey_complete_cancel_cb(void* context) {
    NfcMfkeyRecoverContext* rc = context;
    return rc->cancel;
}

static void nfc_scene_mfkey_complete_collect_key(
    NfcMfkeyRecoverContext* rc,
    uint8_t sector,
    MfClassicKeyType key_type,
    const MfClassicKey* key) {
    // De-dup on the full (sector, key_type, 6-byte key) tuple.
    for(size_t i = 0; i < rc->recovered_count; i++) {
        if(rc->keys[i].sector == sector && rc->keys[i].key_type == key_type &&
           memcmp(rc->keys[i].key.data, key->data, sizeof(MfClassicKey)) == 0) {
            return;
        }
    }
    if(rc->recovered_count >= NFC_MFKEY_RECOVER_MAX_KEYS) return;

    rc->keys[rc->recovered_count].sector = sector;
    rc->keys[rc->recovered_count].key_type = key_type;
    rc->keys[rc->recovered_count].key = *key;
    rc->recovered_count++;
}

static int32_t nfc_scene_mfkey_complete_worker(void* context) {
    NfcApp* instance = context;
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;

    for(size_t i = 0; i < rc->total; i++) {
        if(rc->cancel) break;

        Mfkey32Param param;
        if(mfkey32_logger_get_param(instance->mfkey32_logger, i, &param)) {
            // Heap precheck: recovery needs ~112 KB; skip (don't crash) if the
            // free heap can't safely hold it on this no-PSRAM device.
            if(memmgr_get_free_heap() < NFC_MFKEY_RECOVER_HEAP_MARGIN) {
                rc->skipped_lowmem++;
            } else {
                uint64_t key64 = 0;
                bool ok = mfkey32_recover_cancellable(
                    param.cuid,
                    param.nt0,
                    param.nr0,
                    param.ar0,
                    param.nt1,
                    param.nr1,
                    param.ar1,
                    &key64,
                    nfc_scene_mfkey_complete_cancel_cb,
                    rc);
                if(ok) {
                    // out_key is the 48-bit key MSB-first in the low 6 bytes.
                    MfClassicKey key;
                    for(size_t b = 0; b < sizeof(MfClassicKey); b++) {
                        key.data[b] =
                            (uint8_t)(key64 >> (8 * (sizeof(MfClassicKey) - 1 - b)));
                    }
                    nfc_scene_mfkey_complete_collect_key(
                        rc, param.sector_num, param.key_type, &key);
                }
            }
        }

        rc->processed = i + 1;
        view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventWorkerUpdate);
    }

    rc->running = false;
    view_dispatcher_send_custom_event(instance->view_dispatcher, NfcCustomEventWorkerExit);
    return 0;
}

static void nfc_scene_mfkey_complete_stop_worker(NfcApp* instance) {
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    if(rc->thread) {
        rc->cancel = true;
        furi_thread_join(rc->thread);
        furi_thread_free(rc->thread);
        rc->thread = NULL;
    }
    rc->running = false;
}

/* ---------- dict save (GUI thread) ---------- */

static size_t nfc_scene_mfkey_complete_save_keys(NfcApp* instance) {
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    KeysDict* dict = keys_dict_alloc(
        NFC_APP_MF_CLASSIC_DICT_USER_PATH, KeysDictModeOpenAlways, sizeof(MfClassicKey));

    size_t added = 0;
    for(size_t i = 0; i < rc->recovered_count; i++) {
        if(keys_dict_is_key_present(dict, rc->keys[i].key.data, sizeof(MfClassicKey))) continue;
        if(keys_dict_add_key(dict, rc->keys[i].key.data, sizeof(MfClassicKey))) added++;
    }

    keys_dict_free(dict);
    return added;
}

/* ---------- views ---------- */

static void nfc_scene_mfkey_complete_button_cb(GuiButtonType result, InputType type, void* context) {
    NfcApp* instance = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(instance->view_dispatcher, result);
    }
}

static void nfc_scene_mfkey_complete_finish(NfcApp* instance) {
    const uint32_t prev_scenes[] = {NfcSceneSavedMenu, NfcSceneStart};
    scene_manager_search_and_switch_to_previous_scene_one_of(
        instance->scene_manager, prev_scenes, COUNT_OF(prev_scenes));
}

static void nfc_scene_mfkey_complete_show_prompt(NfcApp* instance) {
    Widget* widget = instance->widget;
    widget_reset(widget);

    size_t params = mfkey32_logger_get_params_num(instance->mfkey32_logger);
    FuriString* str = furi_string_alloc_printf(
        "%zu nonce pair%s saved to file.\nRecover keys on device?",
        params,
        params == 1 ? "" : "s");
    widget_add_string_element(widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Nonces Collected");
    widget_add_string_multiline_element(
        widget, 64, 22, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(str));
    furi_string_free(str);

    widget_add_button_element(
        widget, GuiButtonTypeLeft, "Finish", nfc_scene_mfkey_complete_button_cb, instance);
    widget_add_button_element(
        widget, GuiButtonTypeRight, "Recover", nfc_scene_mfkey_complete_button_cb, instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

static void nfc_scene_mfkey_complete_update_progress(NfcApp* instance) {
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    snprintf(
        rc->progress_text,
        sizeof(rc->progress_text),
        "%zu / %zu  (%zu keys)",
        rc->processed,
        rc->total,
        rc->recovered_count);
    popup_set_text(instance->popup, rc->progress_text, 64, 28, AlignCenter, AlignTop);
}

static void nfc_scene_mfkey_complete_start_recovery(NfcApp* instance) {
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    rc->cancel = false;
    rc->processed = 0;
    rc->skipped_lowmem = 0;
    rc->recovered_count = 0;
    rc->total = mfkey32_logger_get_params_num(instance->mfkey32_logger);

    popup_reset(instance->popup);
    popup_set_header(instance->popup, "Recovering Keys", 64, 6, AlignCenter, AlignTop);
    nfc_scene_mfkey_complete_update_progress(instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewPopup);

    rc->running = true;
    // 8 KB stack: crapto1 recovery is only shallowly recursive; heap holds the
    // heavy state tables. The heap precheck margin also covers this stack.
    rc->thread =
        furi_thread_alloc_ex("NfcMfkeyRecover", 8192, nfc_scene_mfkey_complete_worker, instance);
    furi_thread_start(rc->thread);
}

static void nfc_scene_mfkey_complete_show_results(NfcApp* instance, bool cancelled) {
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    Widget* widget = instance->widget;
    widget_reset(widget);

    FuriString* str = furi_string_alloc();
    if(cancelled) {
        furi_string_cat_printf(str, "Cancelled.\nRecovered %zu key(s).", rc->recovered_count);
    } else {
        furi_string_cat_printf(str, "Recovered %zu key(s).", rc->recovered_count);
    }
    if(rc->skipped_lowmem > 0) {
        furi_string_cat_printf(str, "\n%zu skipped (low mem).", rc->skipped_lowmem);
    }

    if(rc->recovered_count > 0) {
        widget_add_string_element(
            widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Keys Recovered");
        widget_add_string_multiline_element(
            widget, 64, 20, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(str));
        widget_add_button_element(
            widget, GuiButtonTypeLeft, "Finish", nfc_scene_mfkey_complete_button_cb, instance);
        widget_add_button_element(
            widget, GuiButtonTypeRight, "Save", nfc_scene_mfkey_complete_button_cb, instance);
    } else {
        widget_add_string_element(
            widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "No Keys Found");
        widget_add_string_multiline_element(
            widget, 64, 20, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(str));
        widget_add_button_element(
            widget, GuiButtonTypeCenter, "OK", nfc_scene_mfkey_complete_button_cb, instance);
    }
    furi_string_free(str);

    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

static void nfc_scene_mfkey_complete_show_saved(NfcApp* instance, size_t added) {
    Widget* widget = instance->widget;
    widget_reset(widget);

    FuriString* str = furi_string_alloc_printf(
        "Added %zu new key(s)\nto user dictionary.", added);
    widget_add_string_element(widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Saved");
    widget_add_string_multiline_element(
        widget, 64, 22, AlignCenter, AlignTop, FontSecondary, furi_string_get_cstr(str));
    furi_string_free(str);

    widget_add_button_element(
        widget, GuiButtonTypeCenter, "OK", nfc_scene_mfkey_complete_button_cb, instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);
}

/* ---------- scene ---------- */

void nfc_scene_mf_classic_mfkey_complete_on_enter(void* context) {
    NfcApp* instance = context;

    // Start from a clean recovery context (thread is created on demand).
    NfcMfkeyRecoverContext* rc = &instance->mfkey_recover;
    rc->thread = NULL;
    rc->running = false;
    rc->cancel = false;
    rc->total = 0;
    rc->processed = 0;
    rc->skipped_lowmem = 0;
    rc->recovered_count = 0;

    nfc_scene_mfkey_complete_set_state(instance, MfkeyCompleteStatePrompt);
    nfc_scene_mfkey_complete_show_prompt(instance);
}

bool nfc_scene_mf_classic_mfkey_complete_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;
    NfcSceneMfClassicMfKeyCompleteState state = nfc_scene_mfkey_complete_get_state(instance);

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventWorkerUpdate) {
            if(state == MfkeyCompleteStateRecovering) {
                nfc_scene_mfkey_complete_update_progress(instance);
            }
            consumed = true;
        } else if(event.event == NfcCustomEventWorkerExit) {
            // Worker finished (completed or cancelled): capture the cancel flag
            // before stop_worker() forces it, then join + free cleanly.
            bool was_cancelled = instance->mfkey_recover.cancel;
            nfc_scene_mfkey_complete_stop_worker(instance);
            nfc_scene_mfkey_complete_set_state(instance, MfkeyCompleteStateResults);
            nfc_scene_mfkey_complete_show_results(instance, was_cancelled);
            consumed = true;
        } else if(event.event == GuiButtonTypeRight) {
            if(state == MfkeyCompleteStatePrompt) {
                nfc_scene_mfkey_complete_set_state(instance, MfkeyCompleteStateRecovering);
                nfc_scene_mfkey_complete_start_recovery(instance);
            } else if(state == MfkeyCompleteStateResults) {
                size_t added = nfc_scene_mfkey_complete_save_keys(instance);
                dolphin_deed(DolphinDeedNfcKeyAdd);
                nfc_scene_mfkey_complete_set_state(instance, MfkeyCompleteStateSaved);
                nfc_scene_mfkey_complete_show_saved(instance, added);
            }
            consumed = true;
        } else if(event.event == GuiButtonTypeLeft || event.event == GuiButtonTypeCenter) {
            // "Finish" / "OK" on prompt, results (no-keys), or saved screens.
            if(state != MfkeyCompleteStateRecovering) {
                nfc_scene_mfkey_complete_finish(instance);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(state == MfkeyCompleteStateRecovering) {
            // Request cancel; wait for the worker to unwind (WorkerExit) so the
            // thread is joined + freed before we move on. Do not navigate here.
            instance->mfkey_recover.cancel = true;
            consumed = true;
        } else {
            nfc_scene_mfkey_complete_finish(instance);
            consumed = true;
        }
    }

    return consumed;
}

void nfc_scene_mf_classic_mfkey_complete_on_exit(void* context) {
    NfcApp* instance = context;

    // Guarantee the worker is stopped + freed even if we leave mid-recovery
    // (e.g. app exit). No leak / no use-after-free of the shared context.
    nfc_scene_mfkey_complete_stop_worker(instance);

    popup_reset(instance->popup);
    widget_reset(instance->widget);
}
