#include "protopirate_plugins.h"
#include "scenes/protopirate_scene.h"
#include <string.h>

#define TAG "PPPlugins"

// -----------------------------------------------------------------------------
// ESP32 STATIC PORT: there is NO runtime FAP plugin loader on this build.
//
// Upstream ProtoPirate ships every plugin (the five RX protocol registries, the
// per-protocol TX encoders, and all the shared "tool" scenes — config, about,
// saved-info, sub-decode, brute-force, remote-analyzer, welcome) as separate
// .fal ELF modules that shared_plugin_load() maps to memory at runtime.
//
// This port compiles all of those straight into the single app image (see
// application.fam) and resolves them here by calling their stock _ep() directly
// instead of loading a .fal. shared_plugin_unload() therefore has nothing to
// free (the descriptors are compiled-in consts). The public contract
// (ProtoPiratePlugin.plugin_pointer == descriptor->entry_point) is identical to
// the loader build, so every caller is unchanged.
// -----------------------------------------------------------------------------

extern const FlipperAppPluginDescriptor* protopirate_am_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_am_vag_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_f4_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_fm_honda1_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_config_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_saved_info_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_about_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_sub_decode_plugin_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_bruteforce_plugin_ep(void);
#ifdef ENABLE_WELCOME_SCREEN
extern const FlipperAppPluginDescriptor* protopirate_welcome_plugin_ep(void);
#endif
#ifdef ENABLE_TIMING_TUNER_SCENE
extern const FlipperAppPluginDescriptor* protopirate_timing_tuner_plugin_ep(void);
#endif
#ifdef ENABLE_EMULATE_FEATURE
extern const FlipperAppPluginDescriptor* protopirate_emulate_plugin_ep(void);
// Per-protocol TX encoder plugins (static port: one _ep() each, see
// protocols/plugins/protopirate_tx_static_plugins.c). Dispatched by "tx_<key>.fal".
extern const FlipperAppPluginDescriptor* protopirate_tx_chrysler_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_fiat_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_fiat_v1_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_ford_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_ford_v1_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_ford_v2_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_honda_static_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_honda_v1_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v1_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v2_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v3_v4_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v5_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v6_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_kia_v7_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_honda_v2_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_mazda_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_psa_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_renault_v0_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_renault_v1_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_star_line_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_subaru_ep(void);
extern const FlipperAppPluginDescriptor* protopirate_tx_vag_ep(void);
#endif
#ifdef ENABLE_REMOTE_ANALYZER
extern const FlipperAppPluginDescriptor* protopirate_remote_analyzer_plugin_ep(void);
#endif

static const FlipperAppPluginDescriptor*
    protopirate_static_plugin_ep(ProtoPirateSharedPluginIDs plugin_type, const char* txrx_path) {
    switch(plugin_type) {
    case ProtoPirateSharedPluginsConfig:
        return protopirate_config_plugin_ep();
    case ProtoPirateSharedPluginsSavedInfo:
        return protopirate_saved_info_plugin_ep();
    case ProtoPirateSharedPluginsAbout:
        return protopirate_about_plugin_ep();
#ifdef ENABLE_WELCOME_SCREEN
    case ProtoPirateSharedPluginsWelcome:
        return protopirate_welcome_plugin_ep();
#endif
#ifdef ENABLE_EMULATE_FEATURE
    case ProtoPirateSharedPluginsEmulate:
        return protopirate_emulate_plugin_ep();
#endif
    case ProtoPirateSharedPluginsSubDecode:
        return protopirate_sub_decode_plugin_ep();
#ifdef ENABLE_TIMING_TUNER_SCENE
    case ProtoPirateSharedPluginsTimingTuner:
        return protopirate_timing_tuner_plugin_ep();
#endif
    case ProtoPirateSharedPluginsPSABruteforce:
        return protopirate_bruteforce_plugin_ep();
#ifdef ENABLE_REMOTE_ANALYZER
    case ProtoPirateSharedPluginsRemoteAnalyzer:
        return protopirate_remote_analyzer_plugin_ep();
#endif
    case ProtoPirateSharedPluginsTXRX: {
        if(!txrx_path) return NULL;
        if(strcmp(txrx_path, "am_def.fal") == 0) return protopirate_am_plugin_ep();
        if(strcmp(txrx_path, "am_vag.fal") == 0) return protopirate_am_vag_plugin_ep();
        if(strcmp(txrx_path, "fm_def.fal") == 0) return protopirate_fm_plugin_ep();
        if(strcmp(txrx_path, "fm_f4.fal") == 0) return protopirate_fm_f4_plugin_ep();
        if(strcmp(txrx_path, "fm_honda1.fal") == 0) return protopirate_fm_honda1_plugin_ep();
#ifdef ENABLE_EMULATE_FEATURE
        // Per-protocol TX encoder plugins. Upstream loads one tx_<key>.fal on
        // demand; the static port routes the same path strings to the matching
        // compiled-in _ep() (protocols/plugins/protopirate_tx_static_plugins.c).
#define PP_TX_DISPATCH(key) \
    if(strcmp(txrx_path, "tx_" #key ".fal") == 0) return protopirate_tx_##key##_ep();
        PP_TX_DISPATCH(chrysler_v0)
        PP_TX_DISPATCH(fiat_v0)
        PP_TX_DISPATCH(fiat_v1)
        PP_TX_DISPATCH(ford_v0)
        PP_TX_DISPATCH(ford_v1)
        PP_TX_DISPATCH(ford_v2)
        PP_TX_DISPATCH(honda_static)
        PP_TX_DISPATCH(honda_v1)
        PP_TX_DISPATCH(kia_v0)
        PP_TX_DISPATCH(kia_v1)
        PP_TX_DISPATCH(kia_v2)
        PP_TX_DISPATCH(kia_v3_v4)
        PP_TX_DISPATCH(kia_v5)
        PP_TX_DISPATCH(kia_v6)
        PP_TX_DISPATCH(kia_v7)
        PP_TX_DISPATCH(honda_v2)
        PP_TX_DISPATCH(mazda_v0)
        PP_TX_DISPATCH(psa)
        PP_TX_DISPATCH(renault_v0)
        PP_TX_DISPATCH(renault_v1)
        PP_TX_DISPATCH(star_line)
        PP_TX_DISPATCH(subaru)
        PP_TX_DISPATCH(vag)
#undef PP_TX_DISPATCH
        return NULL;
#else
        return NULL;
#endif
    }
    default:
        return NULL;
    }
}

bool shared_plugin_load(
    FlipperApplication** flipper_application_pointer,
    ProtoPiratePlugin* plugin_pointer,
    ProtoPirateSharedPluginIDs plugin_type,
    const char* txrx_path) {
    if(plugin_pointer->plugin_pointer) return true;

    const FlipperAppPluginDescriptor* app_descriptor =
        protopirate_static_plugin_ep(plugin_type, txrx_path);
    if(!app_descriptor || !app_descriptor->entry_point) {
        FURI_LOG_E(TAG, "No static plugin for type %d", (int)plugin_type);
        if(flipper_application_pointer) *flipper_application_pointer = NULL;
        return false;
    }

    plugin_pointer->plugin_pointer = app_descriptor->entry_point;
    // Static build: no FlipperApplication is allocated.
    if(flipper_application_pointer) *flipper_application_pointer = NULL;
    return true;
}

void shared_plugin_unload(
    FlipperApplication** flipper_application_pointer,
    ProtoPiratePlugin* plugin_pointer) {
    // Nothing is mapped to memory in the static build; just drop the pointers.
    if(flipper_application_pointer) *flipper_application_pointer = NULL;
    if(plugin_pointer) plugin_pointer->plugin_pointer = NULL;
}

bool shared_plugin_handle_navigation_events(
    SceneManager* scene_manager,
    ViewDispatcher* view_dispatcher,
    SceneManagerEvent event) {
    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t scene_id = 0;
        if(event.event == ProtoPirateCustomEventPluginNavigateReceiver) {
            scene_id = ProtoPirateSceneReceiver;
        }
        if(event.event == ProtoPirateCustomEventPluginNavigateSwitchToReceiver) {
            scene_manager_previous_scene(scene_manager);
            scene_id = ProtoPirateSceneReceiver;
        }
        if(event.event == ProtoPirateCustomEventPluginNavigateEmulate) {
#ifdef ENABLE_EMULATE_FEATURE
            scene_id = ProtoPirateSceneEmulate;
#endif
        }
        if(event.event == ProtoPirateCustomEventPluginNavigateConfig) {
            scene_id = ProtoPirateSceneReceiverConfig;
        } else if(event.event == ProtoPirateCustomEventPluginNavigateBack) {
            scene_manager_previous_scene(scene_manager);
            return true;
        } else if(event.event == ProtoPirateCustomEventPluginNavigateStopApp) {
            scene_manager_stop(scene_manager);
            view_dispatcher_stop(view_dispatcher);
            return true;
        }

        if(scene_id) {
            scene_manager_next_scene(scene_manager, scene_id);
            return true;
        }
    }
    return false;
}
