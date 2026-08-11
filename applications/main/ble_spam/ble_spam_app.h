#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>

#include "ble_tracker_hal.h"
#include "tracker_category.h"
#include "scenes/scenes.h"

#ifndef PUBLIC_BUILD
// BLE GATT Fuzzer + connection-flood DoS (private-only attack surface; see
// ble_walk_fuzz.h / views/ble_fuzz_view.h). Target Info panel + Fuzz/DoS
// config reuse the generic Widget / VariableItemList modules rather than
// dedicated views (only the run view, ble_fuzz_view, is a bespoke module —
// Task 5).
#include "ble_walk_fuzz.h"
#include "views/ble_fuzz_view.h"
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#endif

#define BLE_SPAM_LOG_TAG "BleSpam"

typedef enum {
    BleSpamAttackAppleDevice,
    BleSpamAttackAppleAction,
    BleSpamAttackAppleNotYourDevice,
    BleSpamAttackFastPair,
    BleSpamAttackSwiftPair,
    BleSpamAttackSamsungBuds,
    BleSpamAttackSamsungWatch,
    BleSpamAttackXiaomi,
    BleSpamAttackPairSpam,
    BleSpamAttackPairSpamRickroll,
    BleSpamAttackPairSpamCustom,
    BleSpamAttackAndroidHammer,
    BleSpamAttackCycleAll,
    BleSpamAttackCount,
} BleSpamAttackType;

typedef enum {
    BleSpamCustomEventToggle = 100,
    BleSpamCustomEventSpeedUp,
    BleSpamCustomEventSpeedDown,
} BleSpamCustomEvent;

typedef enum {
    BleSpamCustomEventWalkConnect = 200,
#ifndef PUBLIC_BUILD
    // BLE GATT Fuzzer/DoS (private-only): Target Info's "Arm DoS" button, and
    // the WalkFuzzRun scene's bridge from BleFuzzViewEvent (view callback ->
    // custom event, same idiom as SubGhzCustomEventViewFuzzer*).
    BleSpamCustomEventWalkTargetInfoArmDos,
    BleSpamCustomEventWalkFuzzStartPause,
    BleSpamCustomEventWalkFuzzStep,
    BleSpamCustomEventWalkFuzzMark,
    BleSpamCustomEventWalkFuzzStop,
#endif
} BleSpamWalkEvent;

typedef enum {
    BleSpamViewSubmenu,
    BleSpamViewRunning,
    BleSpamViewTextInput,
    BleSpamViewWalkScan,
    BleSpamViewWalkDetail,
    BleSpamViewAutoWalk,
    BleSpamViewTrackerScan,
    BleSpamViewTrackerGeiger,
    BleSpamViewRaceDetector,
    BleSpamViewWhisperpairDetector,
#ifndef PUBLIC_BUILD
    // BLE GATT Fuzzer/DoS (private-only). WalkTargetInfo (Widget) and
    // WalkFuzzConfig (VariableItemList) reuse generic modules, so each still
    // needs its own view id for view_dispatcher_add_view/switch_to_view;
    // WalkFuzz (the run view, ble_fuzz_view) is the one bespoke view id the
    // brief calls out by name.
    BleSpamViewWalkTargetInfo,
    BleSpamViewWalkFuzzConfig,
    BleSpamViewFuzz,
#endif
} BleSpamViewId;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    View* view_running;
    TextInput* text_input;

    // Attack state
    BleSpamAttackType attack_type;
    volatile bool running;
    uint32_t packet_count;
    uint32_t delay_ms;
    uint16_t current_index;
    uint16_t cycle_index; // rotor for the MAX Cycle-All attack
    char current_device[48];
    char custom_pair_name[32];

    // BLE Walk state
    View* view_walk_scan;
    View* view_walk_detail;
    uint16_t walk_selected_device;
    uint16_t walk_selected_service;
    uint16_t walk_selected_char;

    // BLE Auto-Walk state
    View* view_auto_walk;

    // BLE Tracker state
    View* view_tracker_scan;
    View* view_tracker_geiger;
    TrackerDevice tracker_target;
    TrackerCategory tracker_category;   // chosen in the picker, read by the scan scene
    FuriTimer* tracker_geiger_timer;
    volatile int8_t tracker_current_rssi;
    volatile bool tracker_current_stale;
    uint32_t tracker_current_period;

    // Airoha RACE Detector state (CVE-2025-20700)
    View* view_race_detector;
    volatile bool race_probe_abort;

    // WhisperPair Detector state (CVE-2025-36911, Fast Pair)
    View* view_whisperpair_detector;
    volatile bool wp_probe_abort;

#ifndef PUBLIC_BUILD
    // BLE GATT Fuzzer + connection-flood DoS state (private-only).
    Widget* widget; // Target Info panel (text-scroll + "Arm DoS" button)
    VariableItemList* fuzz_config_list; // Fuzz/DoS config (mode/strategy/write/rate/count)
    BleWalkFuzz* walk_fuzz; // engine; lazy-alloc'd by scene_walk_fuzz_run on_enter
    BleFuzzView* fuzz_view; // run view (Task 5)
    BleWalkDevice fuzz_target; // locked target, staged by TargetInfo (DoS) or CharDetail (Sweep/Stack)
    uint16_t fuzz_char_handle; // Sweep/Stack target characteristic; ignored by DoS
    BleFuzzMode fuzz_mode;
    FuzzConfig fuzz_cfg; // fuzzer_mutate seed/strategy; .mode is always FuzzModeProtocol here
    bool fuzz_write_norsp;
    uint32_t fuzz_rate_ms;
    uint32_t fuzz_count; // 0 = full sweep / unbounded DoS
#endif
} BleSpamApp;
