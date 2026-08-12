#pragma once

#include "helpers/subghz_types.h"
#include "helpers/subghz_gen_info.h"
#include <lib/subghz/types.h>
#include "subghz.h"
#include "views/receiver.h"
#include "views/transmitter.h"
#include "views/subghz_frequency_analyzer.h"
#include "views/subghz_read_raw.h"
#include "views/subghz_jammer.h"
#include "views/subghz_playlist.h"
#include "views/subbrute_main_view.h"
#include "views/subbrute_attack_view.h"
#include "views/subghz_view_tpms_info.h"
#include "views/subghz_view_protoview.h"
#include "views/subghz_view_spectrum.h"
#include "views/subghz_keeloq_fob.h"
#ifndef PUBLIC_BUILD
#include "helpers/fuzzer_worker.h" // Protocol Fuzzer (PRIVATE-only)
#include "fuzzer_mutate.h"
#include "views/subghz_view_fuzzer.h"
#endif
#include "helpers/subghz_rollback.h"
#include "helpers/subbrute_device.h"
#include "helpers/subbrute_worker.h"
#include "helpers/subbrute_settings.h"
#include "helpers/subbrute_radio_device_loader.h"

#include <gui/gui.h>
#include <assets_icons.h>
#include <dialogs/dialogs.h>
#include <gui/scene_manager.h>
#include <notification/notification_messages.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/number_input.h>
#include <gui/modules/widget.h>

#include <subghz/scenes/subghz_scene.h>
#include <lib/subghz/subghz_worker.h>
#include <lib/subghz/subghz_file_encoder_worker.h>
#include <lib/subghz/subghz_setting.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/transmitter.h>

#include "subghz_history.h"
#include "subghz_last_settings.h"

#include <gui/modules/variable_item_list.h>
#include <lib/toolbox/path.h>

#include "rpc/rpc_app.h"

#include "helpers/subghz_threshold_rssi.h"

#include "helpers/subghz_txrx.h"

#include "helpers/subghz_decode_dedup.h"

#define SUBGHZ_MAX_LEN_NAME      64
#define SUBGHZ_EXT_PRESET_NAME   true
#define SUBGHZ_RAW_THRESHOLD_MIN (-90.0f)
#define SUBGHZ_MEASURE_LOADING   false

struct SubGhz {
    Gui* gui;
    NotificationApp* notifications;

    SubGhzTxRx* txrx;

    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    Popup* popup;
    TextInput* text_input;
    ByteInput* byte_input;
    Widget* widget;
    NumberInput* number_input;
    DialogsApp* dialogs;
    FuriString* file_path;
    FuriString* file_path_tmp;
    char file_name_tmp[SUBGHZ_MAX_LEN_NAME];
    SubGhzNotificationState state_notifications;

    SubGhzViewReceiver* subghz_receiver;
    SubGhzViewTransmitter* subghz_transmitter;
    VariableItemList* variable_item_list;

    SubGhzFrequencyAnalyzer* subghz_frequency_analyzer;
    SubGhzReadRAW* subghz_read_raw;
    SubGhzJammer* subghz_jammer;
    SubGhzKeeloqFob* subghz_keeloq_fob;
    SubGhzPlaylist* subghz_playlist;
    SubGhzViewTpmsInfo* subghz_tpms_info;
    SubGhzViewProtoview* subghz_protoview;
    SubGhzViewSpectrum* subghz_spectrum;
    // TPMS edit-scene state (which field is being edited and the result buffers
    // the NumberInput / ByteInput callbacks write into).
    SubGhzViewTpmsField tpms_edit_field;
    int32_t tpms_edit_number_value;
    uint8_t tpms_edit_id_bytes[4];

    // RollBack rolling-code capture-then-replay
    RollbackStore rollback_store;
    // Serialized .sub body per slot, index-aligned. 512 (was 192) so structured rolling
    // frames (Protocol/Bit/Key + per-protocol fields) fit without the truncate-then-reject
    // that dropped valid captures; over-long bodies are still rejected in subghz_rollback.c.
    char rollback_frame[ROLLBACK_MAX_SLOTS][512];
    volatile bool rollback_abort;

    // SubBrute Bruteforcer
    SubBruteMainView* subbrute_main_view;
    SubBruteAttackView* subbrute_attack_view;
    SubBruteDevice* subbrute_device;
    SubBruteWorker* subbrute_worker;
    SubBruteSettings* subbrute_settings;
    const SubGhzDevice* subbrute_radio_device;

    bool raw_send_only;
    bool export_to_usb;

    bool save_datetime_set;
    DateTime save_datetime;

    SubGhzLastSettings* last_settings;

    SubGhzProtocolFlag filter;
    SubGhzProtocolFlag ignore_filter;
    FuriString* error_str;
    SubGhzLock lock;

    GenInfo* gen_info;

    // TRUE only while a KeeLoq "By Manufacturer" fob is being generated, so the
    // generation-success path offers an Emulate/Save choice instead of going
    // straight to SaveName. Every other Add-Manually / hardcoded generation must
    // leave this false.
    bool keeloq_mfr_emulate_flow;

    // TRUE only while a KeeLoq no-desync clone is being generated (from a capture
    // or the By-Manufacturer flow), so the counter/jump path is taken instead of
    // the normal Emulate. Mirror every clear-site of keeloq_mfr_emulate_flow.
    bool keeloq_clone_flow;
    uint16_t keeloq_clone_jump;

    // Car Keys hub: prior automotive mode, restored on hub exit.
    uint8_t car_keys_prev_car_mode;
    // TRUE when the fob scene is opened from a saved Garage clone (skip rebuild).
    bool keeloq_fob_from_garage;
    // TRUE when the fob scene runs the GENERIC multi-button path (any supported car
    // protocol other than the VAG/KeeLoq special cases) — its buttons come from
    // subghz_fob_buttons() and the scene owns the counter advance/persist. Stays set
    // for the scene's lifetime (press path reads it) and is cleared on scene exit.
    bool keeloq_fob_generic;
    // For the generic fob path: the FlipperFormat button field this protocol reads
    // ("Btn"/"Button"/"BtnLo") and the on-screen title (protocol name). Persist for
    // the app lifetime so the view may hold the title pointer without copying.
    char keeloq_fob_field[16];
    char keeloq_fob_title[24];
    // For the generic fob path: the counter field this protocol's encoder reads
    // ("Cnt"/"Rolling"/"Counter"/"RollingCode", from subghz_fob_counter_field)
    // and the counter value we last transmitted with. The press path advances
    // THIS field; keeloq_fob_cnt_seed lets the post-TX advance tell a self-
    // advancing encoder (e.g. Honda V5, which persists its own counter) from a
    // scene-rolled one (Kia/Ford/Honda V7..) so it neither double-advances nor
    // stalls. Set in the generic on-enter, used in the tick/advance path.
    char keeloq_fob_cnt_field[16];
    uint32_t keeloq_fob_cnt_seed;
    // TRUE when the receiver was entered via "TPMS Spoofer": tune 433.92 FSK +
    // only list TPMS-sensor decodes (private-only scanner). See subghz_scene_receiver.c.
    bool tpms_scan_mode;

    SubGhzFileEncoderWorker* decode_raw_file_worker_encoder;

    SubGhzThresholdRssi* threshold_rssi;
    SubGhzRxKeyState rx_key_state;
    SubGhzHistory* history;
    SubGhzDecodeDedup dedup;

    uint16_t idx_menu_chosen;
    SubGhzLoadTypeFile load_type_file;
    uint8_t tx_power;
    void* rpc_ctx;

#ifndef PUBLIC_BUILD
    // Protocol Fuzzer (PRIVATE-only): filled in by the config scene, consumed
    // by fuzzer_worker_set_config() in the run scene. fuzzer_cfg carries the
    // mutation-side settings (mode/strategy/seed/...); the rest mirror the
    // separate params fuzzer_worker_set_config() takes (protocol/freq/preset
    // aren't part of FuzzConfig). Worker + view are lazy-alloc'd by the run
    // scene (mirrors the Jammer/Spectrum lifecycle — see
    // subghz_scene_protocol_fuzzer_run.c and the safety-net free in
    // subghz_free()).
    FuzzConfig fuzzer_cfg;
    char fuzzer_protocol[16]; // e.g. "Princeton", "CAME", "Nice FLO", "Holtek", "Linear"
    uint32_t fuzzer_freq;
    char fuzzer_preset[40]; // FuriHalSubGhzPreset enum name (subbrute_protocol_convert_preset)
    uint32_t fuzzer_te;
    uint8_t fuzzer_repeat;
    FuzzerWorker* fuzzer_worker;
    SubGhzViewFuzzer* fuzzer_view;
#endif
};

void subghz_blink_start(SubGhz* subghz);
void subghz_blink_stop(SubGhz* subghz);

bool subghz_tx_start(SubGhz* subghz, FlipperFormat* flipper_format);
void subghz_dialog_message_freq_error(SubGhz* subghz, bool only_rx);

bool subghz_key_load(SubGhz* subghz, const char* file_path, bool show_dialog);

// True if a captured/saved fob of `proto` (already loaded into `fff`) should open
// the multi-button rolling fob GRID rather than the plain transmitter: the
// protocol must be subghz_fob_protocol_supported AND pass the per-family guards
// (Honda V2&V3 whose decoded sub-variant is V3 bakes the button into the frame =
// replay; Fiat V1 whose encoder aborts without a "Hitag2 Key" field). Callers that
// take the grid must set keeloq_fob_generic + keeloq_fob_from_garage before
// switching to SubGhzSceneKeeloqFob.
bool subghz_fob_can_emulate(FlipperFormat* fff, const char* proto);
bool subghz_get_next_name_file(SubGhz* subghz, uint8_t max_len);
bool subghz_save_protocol_to_file(
    SubGhz* subghz,
    FlipperFormat* flipper_format,
    const char* dev_file_name);
void subghz_save_to_file(void* context);
bool subghz_load_protocol_from_file(SubGhz* subghz);
bool subghz_rename_file(SubGhz* subghz);
bool subghz_file_available(SubGhz* subghz);
bool subghz_delete_file(SubGhz* subghz);
void subghz_file_name_clear(SubGhz* subghz);
bool subghz_path_is_file(FuriString* path);
SubGhzLoadTypeFile subghz_get_load_type_file(SubGhz* subghz);

void subghz_lock(SubGhz* subghz);
void subghz_unlock(SubGhz* subghz);
bool subghz_is_locked(SubGhz* subghz);

void subghz_rx_key_state_set(SubGhz* subghz, SubGhzRxKeyState state);
SubGhzRxKeyState subghz_rx_key_state_get(SubGhz* subghz);

extern const NotificationSequence subghz_sequence_rx;
extern const NotificationSequence subghz_sequence_rx_locked;
