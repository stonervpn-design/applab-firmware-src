#pragma once

#include "nfc_app.h"

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view.h>
#include <assets_icons.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <notification/notification_messages.h>

#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>
#include "views/dict_attack.h"
#include "views/detect_reader.h"
#include "views/dict_attack.h"

#include <nfc/scenes/nfc_scene.h>
#include "helpers/nfc_detected_protocols.h"
#include "helpers/nfc_custom_event.h"
#include "helpers/mf_ultralight_auth.h"
#include "helpers/mf_user_dict.h"
#include "helpers/mfkey32_logger.h"
#include "helpers/mfkey32_recover.h"
#include "helpers/nfc_emv_parser.h"
#include "helpers/mf_classic_key_cache.h"
#include "helpers/protocol_support/nfc_protocol_support.h"
#include "helpers/nfc_supported_cards.h"
#include "helpers/felica_auth.h"
#include "helpers/slix_unlock.h"
#ifndef PUBLIC_BUILD
#include "helpers/mf_desfire_attack.h"
#include "helpers/mf_desfire_audit.h"
#endif

#include <flipper_application/plugins/composite_resolver.h>
#include <loader/loader.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <toolbox/path.h>

#include "rpc/rpc_app.h"

#include <m-array.h>

#include <nfc/nfc.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_listener.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_listener.h>

#include <nfc/nfc_poller.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_listener.h>

#include <nfc/nfc_device.h>
#include <nfc/helpers/nfc_data_generator.h>
#include <toolbox/keys_dict.h>

#include <gui/modules/validators.h>
#include <toolbox/path.h>
#include <toolbox/name_generator.h>
#include <dolphin/dolphin.h>

#define NFC_NAME_SIZE             22
#define NFC_TEXT_STORE_SIZE       128
#define NFC_BYTE_INPUT_STORE_SIZE 16
#define NFC_LOG_SIZE_MAX          (1024)
#define NFC_APP_FOLDER            EXT_PATH("nfc")
#define NFC_APP_EXTENSION         ".nfc"
#define NFC_APP_SHADOW_EXTENSION  ".shd"
#define NFC_APP_FILENAME_PREFIX   "NFC"

#define NFC_APP_MFKEY32_LOGS_FILE_NAME ".mfkey32.log"
#define NFC_APP_MFKEY32_LOGS_FILE_PATH (NFC_APP_FOLDER "/" NFC_APP_MFKEY32_LOGS_FILE_NAME)

#define NFC_APP_MF_CLASSIC_DICT_USER_PATH (NFC_APP_FOLDER "/assets/mf_classic_dict_user.nfc")
#define NFC_APP_MF_CLASSIC_DICT_USER_NESTED_PATH \
    (NFC_APP_FOLDER "/assets/mf_classic_dict_user_nested.nfc")
#define NFC_APP_MF_CLASSIC_DICT_SYSTEM_PATH (NFC_APP_FOLDER "/assets/mf_classic_dict.nfc")
#define NFC_APP_MF_CLASSIC_DICT_SYSTEM_NESTED_PATH \
    (NFC_APP_FOLDER "/assets/mf_classic_dict_nested.nfc")
#define NFC_APP_MF_ULTRALIGHT_C_DICT_USER_PATH \
    (NFC_APP_FOLDER "/assets/mf_ultralight_c_dict_user.nfc")
#define NFC_APP_MF_ULTRALIGHT_C_DICT_SYSTEM_PATH \
    (NFC_APP_FOLDER "/assets/mf_ultralight_c_dict.nfc")

#define NFC_MFKEY32_APP_PATH (EXT_PATH("apps/NFC/mfkey.fap"))

typedef enum {
    NfcRpcStateIdle,
    NfcRpcStateEmulating,
} NfcRpcState;

typedef struct {
    KeysDict* dict;
    uint8_t sectors_total;
    uint8_t sectors_read;
    uint8_t current_sector;
    uint8_t keys_found;
    size_t dict_keys_total;
    size_t dict_keys_current;
    bool is_key_attack;
    uint8_t key_attack_current_sector;
    bool is_card_present;
    MfClassicNestedPhase nested_phase;
    MfClassicPrngType prng_type;
    MfClassicBackdoor backdoor;
    uint16_t nested_target_key;
    uint16_t msb_count;
    bool enhanced_dict;
    uint16_t current_key_idx; // Current key index for CUID dictionary mode
    uint8_t*
        cuid_key_indices_bitmap; // Bitmap of key indices present in CUID dictionary (256 bits = 32 bytes)
} NfcMfClassicDictAttackContext;

typedef struct {
    KeysDict* dict;
    bool auth_success;
    bool is_card_present;
    size_t dict_keys_total;
    size_t dict_keys_current;
} NfcMfUltralightCDictContext;

// On-device mfkey32 key recovery (worker-thread) context.
// MIFARE Classic 4K has 40 sectors * 2 key types = 80 unique (sector,key_type).
#define NFC_MFKEY_RECOVER_MAX_KEYS (80)
// Each recovery needs ~112 KB heap; skip a set if free heap is below this margin
// (leaves headroom for the GUI stack/allocations on a no-PSRAM ESP32-S3).
#define NFC_MFKEY_RECOVER_HEAP_MARGIN (135 * 1024)

typedef struct {
    uint8_t sector;
    MfClassicKeyType key_type;
    MfClassicKey key;
} NfcMfkeyRecoveredKey;

typedef struct {
    FuriThread* thread;
    volatile bool cancel; // set by GUI thread; polled by worker + cancel cb
    volatile bool running; // true while worker thread is alive
    size_t total; // number of param sets to process
    size_t processed; // param sets processed so far (progress)
    size_t skipped_lowmem; // sets skipped due to insufficient free heap
    size_t recovered_count; // count of unique keys collected
    NfcMfkeyRecoveredKey keys[NFC_MFKEY_RECOVER_MAX_KEYS];
    char progress_text[32]; // backing store for popup text (must outlive draw)
} NfcMfkeyRecoverContext;

typedef enum {
    NfcMfUltralightCWriteDictIdle, /**< No dict open; safe to open either dict. */
    NfcMfUltralightCWriteDictUser, /**< User dict currently open. */
    NfcMfUltralightCWriteDictSystem, /**< System dict currently open. */
    NfcMfUltralightCWriteDictExhausted, /**< All dicts tried; do not re-open. */
} NfcMfUltralightCWriteDictState;

typedef struct {
    bool copy_key; /**< True = overwrite target 3DES key with source key pages. */
    NfcMfUltralightCWriteDictState dict_state; /**< Which dict is open for write-phase auth. */
} NfcMfUltralightCWriteContext;

#ifndef PUBLIC_BUILD
// DESFire dictionary/key-sweep attack (Task 9, PRIVATE-only). Sweeps either
// the built-in factory-default dictionary (mf_desfire_dict_builtin) or a
// single manually entered key (manual_mode) against every discovered AID's
// key slots via mf_desfire_attack_sweep, then re-authenticates and dumps
// Standard/Backup files for each recovered key. See
// scenes/nfc_scene_mf_desfire_dict_attack.c and
// scenes/nfc_scene_mf_desfire_unlock_menu.c.
#define NFC_MF_DESFIRE_ATTACK_MAX_FOUND (16)

typedef struct {
    // Set by the unlock menu's "Enter key manually" byte input before
    // entering the dict-attack scene; otherwise the built-in dictionary is
    // swept instead.
    bool manual_mode;
    MfDesfireKeyType manual_key_type;
    uint8_t manual_key[MF_DESFIRE_CRYPTO_KEY_MAX];

    // Live progress, updated from the poller worker callback and read back
    // by the scene's event handler on NfcCustomEventDesfireAttackUpdate.
    size_t attempts_done;
    size_t attempts_total;
    bool sweep_started;
    bool sweep_complete;

    // Results, valid once sweep_complete is true.
    MfDesfireFoundKey found[NFC_MF_DESFIRE_ATTACK_MAX_FOUND];
    size_t found_count;
    size_t files_dumped;
} NfcMfDesfireAttackContext;
#endif

struct NfcApp {
    DialogsApp* dialogs;
    Storage* storage;
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    SceneManager* scene_manager;

    char text_store[NFC_TEXT_STORE_SIZE + 1];
    FuriString* text_box_store;
    uint8_t byte_input_store[NFC_BYTE_INPUT_STORE_SIZE];

    NfcDetectedProtocols* detected_protocols;

    RpcAppSystem* rpc_ctx;
    NfcRpcState rpc_state;

    // Common Views
    Submenu* submenu;
    DialogEx* dialog_ex;
    Popup* popup;
    Loading* loading;
    TextInput* text_input;
    ByteInput* byte_input;
    TextBox* text_box;
    Widget* widget;
    DetectReader* detect_reader;
    DictAttack* dict_attack;

    Nfc* nfc;
    NfcPoller* poller;
    NfcScanner* scanner;
    NfcListener* listener;

    FelicaAuthenticationContext* felica_auth;
    MfUltralightAuth* mf_ul_auth;
    SlixUnlock* slix_unlock;
    NfcMfClassicDictAttackContext nfc_dict_context;
    NfcMfUltralightCDictContext mf_ultralight_c_dict_context;
    NfcMfUltralightCWriteContext mf_ultralight_c_write_context;
#ifndef PUBLIC_BUILD
    NfcMfDesfireAttackContext mf_desfire_attack;
#endif
    Mfkey32Logger* mfkey32_logger;
    NfcMfkeyRecoverContext mfkey_recover;
    MfUserDict* mf_user_dict;
    MfClassicKeyCache* mfc_key_cache;
    CompositeApiResolver* api_resolver;
    NfcProtocolSupport* protocol_support;
    NfcSupportedCards* nfc_supported_cards;

    NfcDevice* nfc_device;
    Iso14443_3aData* iso14443_3a_edit_data;
    FuriString* file_path;
    FuriString* file_name;
    FuriTimer* timer;

    /* "Clone" flow: read a card, then go straight to the "Place blank Card"
     * dialog (Write / Wipe / Chameleon) instead of the normal ReadSuccess UI. */
    bool clone_mode;
};

typedef enum {
    NfcViewMenu,
    NfcViewDialogEx,
    NfcViewPopup,
    NfcViewLoading,
    NfcViewTextInput,
    NfcViewByteInput,
    NfcViewTextBox,
    NfcViewWidget,
    NfcViewDictAttack,
    NfcViewDetectReader,
} NfcView;

typedef enum {
    NfcSceneSaveConfirmStateDetectReader,
    NfcSceneSaveConfirmStateCrackNonces,
} NfcSceneSaveConfirmState;

#ifdef __cplusplus
extern "C" {
#endif

int32_t nfc_task(void* p);

void nfc_text_store_set(NfcApp* nfc, const char* text, ...);

void nfc_text_store_clear(NfcApp* nfc);

void nfc_blink_read_start(NfcApp* nfc);

void nfc_blink_emulate_start(NfcApp* nfc);

void nfc_blink_detect_start(NfcApp* nfc);

void nfc_blink_stop(NfcApp* nfc);

void nfc_show_loading_popup(void* context, bool show);

bool nfc_has_shadow_file(NfcApp* instance);

bool nfc_save_shadow_file(NfcApp* instance);

bool nfc_delete_shadow_file(NfcApp* instance);

bool nfc_save(NfcApp* instance);

bool nfc_delete(NfcApp* instance);

bool nfc_load_from_file_select(NfcApp* instance);

bool nfc_load_file(NfcApp* instance, FuriString* path, bool show_dialog);

bool nfc_save_file(NfcApp* instance, FuriString* path);

void nfc_make_app_folder(NfcApp* instance);

void nfc_append_filename_string_when_present(NfcApp* instance, FuriString* string);

void nfc_app_run_external(NfcApp* nfc, const char* app_path);

#ifdef __cplusplus
}
#endif
