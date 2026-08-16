#include "../nfc_app_i.h"

#include <nfc/protocols/mf_desfire/mf_desfire_poller.h>

#ifndef PUBLIC_BUILD
// DESFire dictionary/key-sweep attack scene (Task 9, PRIVATE-only).
//
// Runs a single MfDesfire poller session. The scene puts the poller into
// Attack mode (mf_desfire_poller_set_mode, Task 10b) BEFORE nfc_poller_start,
// so once the poller's read FSM (mf_desfire_poller.c) completes its standard
// read (version, free memory, master key settings, application ids/applications)
// it fires MfDesfirePollerEventTypeRequestAttack *while the iso14443_4a poller
// is still activated* — RF field up, card selected — and BEFORE the terminating
// iso14443_4a_poller_halt(). On that event we get a live MfDesfirePoller*
// (event.instance) and run the actual sweep: mf_desfire_attack_sweep() calls our
// try_auth for every (AID, key_no, dict entry), which selects the AID
// (mf_desfire_poller_select_application) then authenticates
// (mf_desfire_poller_authenticate). For each key found, we re-select +
// re-authenticate and dump Standard/Backup files via
// mf_desfire_poller_read_file_data_authed.
//
// FIX (Task 10b): previously the poller called iso14443_4a_poller_halt() (real
// HLTA + reset to Idle) BEFORE invoking the callback at ReadSuccess/ReadFailed,
// so every select/authenticate targeted an already-halted card and the sweep
// always found 0 keys even with the correct key present. The pre-halt Attack
// hook resolves that: select/authenticate now transceive to a live card.
// Functional key recovery still needs a real-card HW test.

// DESFire AIDs are 24-bit; mf_desfire_attack_sweep's "aid" parameter is only
// 16-bit. We repurpose it as an index into a local real-AID table built
// fresh for every sweep, rather than encoding the literal AID (which could
// collide/truncate). Index 0 is always the PICC master (000000); indices
// 1..N are index-aligned with the just-read data->application_ids /
// data->applications arrays.
#define NFC_MF_DESFIRE_ATTACK_MAX_AIDS (33) // 1 (PICC master) + up to 32 apps

typedef struct {
    MfDesfirePoller* poller;
    const MfDesfireApplicationId* real_aids;
    size_t real_aid_count;
} NfcDesfireTryAuthCtx;

static bool nfc_scene_mf_desfire_dict_attack_try_auth(
    void* ctx,
    uint16_t aid_token,
    uint8_t key_no,
    MfDesfireKeyType type,
    const uint8_t* key) {
    NfcDesfireTryAuthCtx* actx = ctx;
    if(aid_token >= actx->real_aid_count) return false;

    if(mf_desfire_poller_select_application(actx->poller, &actx->real_aids[aid_token]) !=
       MfDesfireErrorNone) {
        return false;
    }
    return mf_desfire_poller_authenticate(actx->poller, key_no, type, key) == MfDesfireErrorNone;
}

static void nfc_scene_mf_desfire_dict_attack_progress(void* p, size_t done, size_t total) {
    NfcApp* instance = p;
    instance->mf_desfire_attack.attempts_done = done;
    instance->mf_desfire_attack.attempts_total = total;
    // Throttle GUI updates: redraw every 8 attempts and on the very last one,
    // so a multi-hundred-attempt sweep doesn't flood the view dispatcher's
    // custom event queue.
    if(done % 8 == 0 || done == total) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDesfireAttackUpdate);
    }
}

static const char* nfc_desfire_attack_key_type_name(MfDesfireKeyType type) {
    switch(type) {
    case MfDesfireKeyTypeDes:
        return "DES";
    case MfDesfireKeyType2k3Des:
        return "2K3DES";
    case MfDesfireKeyType3k3Des:
        return "3K3DES";
    case MfDesfireKeyTypeAes:
        return "AES";
    default:
        return "?";
    }
}

// Runs the sweep + best-effort file dump. Must be called from inside the
// poller worker callback (poller must be a live MfDesfirePoller* for this
// session). Populates instance->mf_desfire_attack with results.
static void nfc_scene_mf_desfire_dict_attack_run(NfcApp* instance, MfDesfirePoller* poller) {
    // Use the generic NfcPoller wrapper's get_data accessor (same pattern as
    // nfc_scene_mf_classic_dict_attack.c) rather than the protocol-internal
    // mf_desfire_poller_get_data (declared in mf_desfire_poller_i.h, not the
    // public mf_desfire_poller.h this file includes).
    const MfDesfireData* data = nfc_poller_get_data(instance->poller);

    // Persist the poller's standard-read data (version/app ids/applications)
    // into the device model now, so the results view (running later, on the
    // GUI thread) can look up real AID bytes via nfc_device_get_data().
    nfc_device_set_data(instance->nfc_device, NfcProtocolMfDesfire, data);

    static const MfDesfireApplicationId k_picc_master = {.data = {0, 0, 0}};
    MfDesfireApplicationId real_aids[NFC_MF_DESFIRE_ATTACK_MAX_AIDS];
    uint16_t aid_tokens[NFC_MF_DESFIRE_ATTACK_MAX_AIDS];
    size_t aid_count = 0;

    real_aids[aid_count] = k_picc_master;
    aid_tokens[aid_count] = (uint16_t)aid_count;
    aid_count++;

    uint32_t app_total = simple_array_get_count(data->application_ids);
    for(uint32_t i = 0; i < app_total && aid_count < NFC_MF_DESFIRE_ATTACK_MAX_AIDS; i++) {
        const MfDesfireApplicationId* app_id = simple_array_cget(data->application_ids, i);
        real_aids[aid_count] = *app_id;
        aid_tokens[aid_count] = (uint16_t)aid_count;
        aid_count++;
    }

    NfcDesfireTryAuthCtx try_ctx = {
        .poller = poller,
        .real_aids = real_aids,
        .real_aid_count = aid_count,
    };

    const MfDesfireDictEntry* dict = NULL;
    size_t dict_count = 0;
    MfDesfireDictEntry manual_entry;
    if(instance->mf_desfire_attack.manual_mode) {
        manual_entry.key = instance->mf_desfire_attack.manual_key;
        manual_entry.type = instance->mf_desfire_attack.manual_key_type;
        dict = &manual_entry;
        dict_count = 1;
    } else {
        dict_count = mf_desfire_dict_builtin(&dict);
    }

    instance->mf_desfire_attack.sweep_started = true;
    instance->mf_desfire_attack.found_count = mf_desfire_attack_sweep(
        nfc_scene_mf_desfire_dict_attack_try_auth,
        &try_ctx,
        aid_tokens,
        aid_count,
        MF_DESFIRE_MAX_KEYS,
        dict,
        dict_count,
        instance->mf_desfire_attack.found,
        NFC_MF_DESFIRE_ATTACK_MAX_FOUND,
        nfc_scene_mf_desfire_dict_attack_progress,
        instance);

    // Best-effort dump: for each found key, re-select + re-authenticate and
    // read Standard/Backup files only (Value/Record/CyclicRecord/
    // TransactionMac files use different read APIs -
    // mf_desfire_poller_read_file_value / _read_file_records - out of scope
    // here). Dumped bytes are shown as a hex preview in the results text;
    // they are NOT merged back into the MfDesfireApplication.file_data
    // SimpleArray model (deferred - see task-9-report.md).
    instance->mf_desfire_attack.files_dumped = 0;
    for(size_t i = 0; i < instance->mf_desfire_attack.found_count; i++) {
        MfDesfireFoundKey* fk = &instance->mf_desfire_attack.found[i];
        if(fk->aid == 0 || fk->aid >= aid_count) continue; // PICC master has no files
        size_t app_idx = fk->aid - 1;
        const MfDesfireApplication* app = simple_array_cget(data->applications, app_idx);

        if(mf_desfire_poller_select_application(poller, &real_aids[fk->aid]) != MfDesfireErrorNone)
            continue;
        if(mf_desfire_poller_authenticate(poller, fk->key_no, fk->type, fk->key) !=
           MfDesfireErrorNone)
            continue;

        uint32_t file_total = simple_array_get_count(app->file_ids);
        for(uint32_t f = 0; f < file_total; f++) {
            const MfDesfireFileSettings* fs = simple_array_cget(app->file_settings, f);
            if(fs->type != MfDesfireFileTypeStandard && fs->type != MfDesfireFileTypeBackup) {
                continue;
            }
            const MfDesfireFileId* file_id = simple_array_cget(app->file_ids, f);

            uint8_t buf[256];
            uint32_t size = fs->data.size;
            if(size > sizeof(buf)) size = sizeof(buf);
            uint32_t out_len = 0;
            MfDesfireError err = mf_desfire_poller_read_file_data_authed(
                poller, *file_id, fs->comm, 0, size, buf, &out_len);
            if(err == MfDesfireErrorNone && out_len > 0) {
                instance->mf_desfire_attack.files_dumped++;
            }
        }
    }

    instance->mf_desfire_attack.sweep_complete = true;
}

static void nfc_scene_mf_desfire_dict_attack_update_view(NfcApp* instance) {
    NfcMfDesfireAttackContext* attack = &instance->mf_desfire_attack;
    furi_string_reset(instance->text_box_store);

    if(!attack->sweep_complete) {
        furi_string_cat_printf(
            instance->text_box_store,
            "Scanning card...\n\nTried: %zu / %zu\nFound: %zu",
            attack->attempts_done,
            attack->attempts_total,
            attack->found_count);
    } else {
        furi_string_cat_printf(
            instance->text_box_store,
            "Attempts: %zu\nKeys found: %zu\nFiles dumped: %zu\n",
            attack->attempts_done,
            attack->found_count,
            attack->files_dumped);

        const MfDesfireData* data = nfc_device_get_data(instance->nfc_device, NfcProtocolMfDesfire);
        for(size_t i = 0; i < attack->found_count; i++) {
            MfDesfireFoundKey* fk = &attack->found[i];
            const char* type_name = nfc_desfire_attack_key_type_name(fk->type);

            if(fk->aid == 0) {
                furi_string_cat_printf(
                    instance->text_box_store, "\nPICC Master key#%u (%s):\n", fk->key_no, type_name);
            } else {
                uint32_t app_idx = fk->aid - 1;
                if(data && app_idx < simple_array_get_count(data->application_ids)) {
                    const MfDesfireApplicationId* app_id =
                        simple_array_cget(data->application_ids, app_idx);
                    furi_string_cat_printf(
                        instance->text_box_store,
                        "\nApp %02x%02x%02x key#%u (%s):\n",
                        app_id->data[2],
                        app_id->data[1],
                        app_id->data[0],
                        fk->key_no,
                        type_name);
                } else {
                    furi_string_cat_printf(
                        instance->text_box_store,
                        "\nAID#%u key#%u (%s):\n",
                        fk->aid,
                        fk->key_no,
                        type_name);
                }
            }

            size_t key_len = mf_desfire_key_size(fk->type);
            for(size_t b = 0; b < key_len; b++) {
                furi_string_cat_printf(instance->text_box_store, "%02X", fk->key[b]);
            }
            furi_string_cat_printf(instance->text_box_store, "\n");
        }

        if(attack->found_count == 0) {
            furi_string_cat_printf(instance->text_box_store, "\nNo dictionary key matched.");
        }
    }

    widget_reset(instance->widget);
    widget_add_text_scroll_element(
        instance->widget, 0, 0, 128, 64, furi_string_get_cstr(instance->text_box_store));
}

NfcCommand nfc_scene_mf_desfire_dict_attack_worker_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.protocol == NfcProtocolMfDesfire);

    NfcApp* instance = context;
    MfDesfirePollerEvent* desfire_event = event.event_data;

    if(desfire_event->type == MfDesfirePollerEventTypeRequestAttack) {
        // Pre-halt hook: the iso14443_4a poller is still activated (RF field up,
        // card selected) and the standard read has populated the model. Running
        // the sweep HERE means every mf_desfire_poller_select_application /
        // mf_desfire_poller_authenticate transceives to a LIVE card, so a correct
        // dictionary key actually authenticates (the pre-halt fix for Task 10b).
        MfDesfirePoller* poller = event.instance;
        nfc_scene_mf_desfire_dict_attack_run(instance, poller);
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDesfireAttackComplete);
    } else if(desfire_event->type == MfDesfirePollerEventTypeReadFailed) {
        // iso14443_4a-level activation error (card removed / never activated):
        // there is no live session to attack. Finish the scene WITHOUT touching
        // the device model (Minor: a failed re-poll must not clobber a
        // previously-good MfDesfireData).
        instance->mf_desfire_attack.sweep_complete = true;
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, NfcCustomEventDesfireAttackComplete);
    }

    // Single-shot: the whole sweep runs synchronously within this one
    // callback invocation, so we always stop the poller session afterward.
    return NfcCommandStop;
}

void nfc_scene_mf_desfire_dict_attack_on_enter(void* context) {
    NfcApp* instance = context;

    instance->mf_desfire_attack.attempts_done = 0;
    instance->mf_desfire_attack.attempts_total = 0;
    instance->mf_desfire_attack.found_count = 0;
    instance->mf_desfire_attack.files_dumped = 0;
    instance->mf_desfire_attack.sweep_started = false;
    instance->mf_desfire_attack.sweep_complete = false;

    widget_reset(instance->widget);
    widget_add_text_scroll_element(
        instance->widget,
        0,
        0,
        128,
        64,
        "Hold card next\nto Flipper's back\n\nScanning...");
    view_dispatcher_switch_to_view(instance->view_dispatcher, NfcViewWidget);

    notification_message(instance->notifications, &sequence_blink_start_yellow);
    nfc_blink_read_start(instance);

    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfDesfire);
    // Enter Attack mode BEFORE starting the session so the poller fires its
    // pre-halt MfDesfirePollerEventTypeRequestAttack hook (field still up, card
    // selected) instead of the post-halt ReadSuccess/ReadFailed events. The tail
    // poller instance exists immediately after nfc_poller_alloc().
    mf_desfire_poller_set_mode(
        nfc_poller_get_poller(instance->poller), MfDesfirePollerModeAttack);
    nfc_poller_start(instance->poller, nfc_scene_mf_desfire_dict_attack_worker_callback, instance);
}

bool nfc_scene_mf_desfire_dict_attack_on_event(void* context, SceneManagerEvent event) {
    NfcApp* instance = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == NfcCustomEventDesfireAttackUpdate) {
            nfc_scene_mf_desfire_dict_attack_update_view(instance);
            consumed = true;
        } else if(event.event == NfcCustomEventDesfireAttackComplete) {
            nfc_scene_mf_desfire_dict_attack_update_view(instance);
            if(instance->mf_desfire_attack.found_count > 0) {
                notification_message(instance->notifications, &sequence_success);
                // Award XP: same deed nfc_scene_mf_classic_dict_attack.c uses on a
                // successful dictionary key recovery (dolphin_deed() is the global
                // helper in dolphin.h; it opens/closes RECORD_DOLPHIN internally,
                // so no record handle to manage here). Emitted from the GUI/scene
                // thread (on_event), not the poller worker callback, matching the
                // existing pattern.
                dolphin_deed(DolphinDeedNfcReadSuccess);
            } else {
                notification_message(instance->notifications, &sequence_semi_success);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        // NOTE: mf_desfire_attack_sweep() has no cancel/interrupt hook (Task
        // 8 API), so this can only abandon the view once the (synchronous,
        // single-callback) sweep has already finished -- it cannot interrupt
        // an in-progress sweep. See task-9-report.md.
        consumed = scene_manager_search_and_switch_to_previous_scene(
            instance->scene_manager, NfcSceneMfDesfireUnlockMenu);
    }

    return consumed;
}

void nfc_scene_mf_desfire_dict_attack_on_exit(void* context) {
    NfcApp* instance = context;

    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);

    widget_reset(instance->widget);
    furi_string_reset(instance->text_box_store);

    nfc_blink_stop(instance);
}

#endif /* !PUBLIC_BUILD — DESFire dict-attack scene is PRIVATE-only */
