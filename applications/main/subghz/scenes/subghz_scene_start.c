#include "../subghz_i.h"
#include "subghz_scene_start.h"
#include <dolphin/dolphin.h>

#include <lib/subghz/protocols/raw.h>

#ifndef PUBLIC_BUILD
#include "../helpers/subbrute_resume.h" // resumable brute sessions (PRIVATE-only)
#endif

void subghz_scene_start_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

void subghz_scene_start_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->state_notifications == SubGhzNotificationStateStarting) {
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    }

    submenu_add_item(
        subghz->submenu, "Read", SubmenuIndexRead, subghz_scene_start_submenu_callback, subghz);
#ifndef PUBLIC_BUILD
    submenu_add_item(
        subghz->submenu,
        "Car Keys",
        SubmenuIndexCarKeys,
        subghz_scene_start_submenu_callback,
        subghz);
#endif
    submenu_add_item(
        subghz->submenu,
        "Read RAW",
        SubmenuIndexReadRAW,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Playlist",
        SubmenuIndexPlaylist,
        subghz_scene_start_submenu_callback,
        subghz);
#ifndef PUBLIC_BUILD
    submenu_add_item(
        subghz->submenu,
        "Jammer",
        SubmenuIndexJammer,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Protocol Fuzzer",
        SubmenuIndexProtocolFuzzer,
        subghz_scene_start_submenu_callback,
        subghz);
#endif
    submenu_add_item(
        subghz->submenu,
        "Bruteforce",
        SubmenuIndexBruteforce,
        subghz_scene_start_submenu_callback,
        subghz);
#ifndef PUBLIC_BUILD
    // Resumable brute (PRIVATE-only): only offered when a saved checkpoint exists.
    if(subbrute_resume_has()) {
        submenu_add_item(
            subghz->submenu,
            "Resume Brute",
            SubmenuIndexResumeBrute,
            subghz_scene_start_submenu_callback,
            subghz);
    }
#endif
#ifndef PUBLIC_BUILD
    submenu_add_item(
        subghz->submenu,
        "RollBack",
        SubmenuIndexRollBack,
        subghz_scene_start_submenu_callback,
        subghz);
#endif
    submenu_add_item(
        subghz->submenu,
        "Signal Decoder",
        SubmenuIndexSignalDecoder,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu, "Saved", SubmenuIndexSaved, subghz_scene_start_submenu_callback, subghz);
#ifndef PUBLIC_BUILD
    submenu_add_item(
        subghz->submenu,
        "Vehicle Vulns",
        SubmenuIndexVehicleVulns,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "TPMS Spoofer",
        SubmenuIndexTpmsSpoof,
        subghz_scene_start_submenu_callback,
        subghz);
#endif
    submenu_add_item(
        subghz->submenu,
        "SubGhz Toolkit",
        SubmenuIndexToolkit,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually",
        SubmenuIndexAddManually,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually [Advanced]",
        SubmenuIndexAddManuallyAdvanced,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Frequency Analyzer",
        SubmenuIndexFrequencyAnalyzer,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Fob Analyzer",
        SubmenuIndexFobAnalyzer,
        subghz_scene_start_submenu_callback,
        subghz);
#ifndef PUBLIC_BUILD
    submenu_add_item(
        subghz->submenu,
        "Spectrum",
        SubmenuIndexSpectrum,
        subghz_scene_start_submenu_callback,
        subghz);
#endif
    submenu_add_item(
        subghz->submenu,
        "Radio Settings",
        SubmenuIndexExtSettings,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_set_selected_item(
        subghz->submenu, scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneStart));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_start_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeBack) {
        //exit app
        scene_manager_stop(subghz->scene_manager);
        view_dispatcher_stop(subghz->view_dispatcher);
        return true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexReadRAW) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexReadRAW);
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReadRAW);
            return true;
        } else if(event.event == SubmenuIndexRead) {
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexRead);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        }
#ifndef PUBLIC_BUILD
        else if(event.event == SubmenuIndexCarKeys) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexCarKeys);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneCarKeys);
            return true;
        }
#endif
        else if(event.event == SubmenuIndexPlaylist) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexPlaylist);
            scene_manager_next_scene(subghz->scene_manager, SubGhzScenePlaylist);
            return true;
        }
#ifndef PUBLIC_BUILD
        else if(event.event == SubmenuIndexJammer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexJammer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneJammer);
            return true;
        } else if(event.event == SubmenuIndexProtocolFuzzer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexProtocolFuzzer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneProtocolFuzzerConfig);
            return true;
        }
#endif
        else if(event.event == SubmenuIndexBruteforce) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexBruteforce);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneBfStart);
            return true;
        }
#ifndef PUBLIC_BUILD
        else if(event.event == SubmenuIndexResumeBrute) {
            // Resume a saved brute checkpoint (PRIVATE-only): reconstruct the
            // attack exactly like bf_start's MenuSelected path, seed the saved
            // step, then jump straight to the setup/run scene. On low heap the
            // SubBrute engine is NULL — guard, and no-op if the record is gone.
            SubBruteResume r;
            if(subghz->subbrute_device && subghz->subbrute_worker &&
               subghz->subbrute_settings && subbrute_resume_load(&r)) {
                subghz->subbrute_settings->last_index = r.attack;
                if(r.attack == SubBruteAttackLoadFile) {
                    // LoadFile resume: a byte/masked file sweep can't be rebuilt
                    // from step alone — the protocol/frequency/preset/bits and the
                    // captured key all live in the source .sub. Reload it, verify
                    // the key still matches (guards an edited/replaced file), then
                    // re-seed the selected group, two-bytes flag, saved mask, and
                    // step. Any failure falls through and simply doesn't resume.
                    SubBruteDevice* d = subghz->subbrute_device;
                    if(subbrute_device_load_from_file(d, r.file_path) ==
                           SubBruteFileResultOk &&
                       d->key_from_file == r.file_key &&
                       subbrute_device_attack_set(d, r.attack, r.extra_repeats) ==
                           SubBruteFileResultOk) {
                        d->bit_index = r.load_index;
                        d->two_bytes = r.two_bytes ? true : false;
                        d->current_step = r.current_step;
                        // Re-engage the saved function mask (if any) BEFORE init so
                        // init_file_attack sizes the 2^k space, not the byte one.
                        subbrute_worker_set_func_mask(
                            subghz->subbrute_worker,
                            r.mask_var,
                            r.mask_active ? true : false);
                        if(subbrute_worker_init_file_attack(
                               subghz->subbrute_worker,
                               r.current_step,
                               d->bit_index,
                               d->key_from_file,
                               d->file_protocol_info,
                               r.extra_repeats,
                               d->two_bytes)) {
                            scene_manager_set_scene_state(
                                subghz->scene_manager,
                                SubGhzSceneStart,
                                SubmenuIndexResumeBrute);
                            scene_manager_next_scene(
                                subghz->scene_manager, SubGhzSceneBfSetupAttack);
                        }
                    }
                } else if(subbrute_device_attack_set(
                       subghz->subbrute_device, r.attack, r.extra_repeats) ==
                   SubBruteFileResultOk) {
                    subghz->subbrute_device->current_step = r.current_step;
                    if(subbrute_worker_init_default_attack(
                           subghz->subbrute_worker,
                           r.attack,
                           r.current_step,
                           subghz->subbrute_device->protocol_info,
                           subghz->subbrute_device->extra_repeats)) {
                        // Restore the PT2262 lift/button code (opencode). init_default_attack
                        // resets it to the protocol default (0), so without this a resumed
                        // sweep TXes the wrong gate suffix and can't actuate the target.
                        uint8_t oc = (r.opencode <= 8) ? r.opencode : 0; // guard the names[9] index
                        subghz->subbrute_device->opencode = oc;
                        subbrute_worker_set_opencode(subghz->subbrute_worker, oc);
                        scene_manager_set_scene_state(
                            subghz->scene_manager,
                            SubGhzSceneStart,
                            SubmenuIndexResumeBrute);
                        scene_manager_next_scene(
                            subghz->scene_manager, SubGhzSceneBfSetupAttack);
                    }
                }
            }
            return true;
        }
#endif
        else if(event.event == SubmenuIndexSaved) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSaved);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaved);
            return true;
        } else if(event.event == SubmenuIndexAddManually) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexAddManually);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetType);
            return true;
        } else if(event.event == SubmenuIndexAddManuallyAdvanced) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexAddManuallyAdvanced);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetType);
            return true;
        } else if(event.event == SubmenuIndexFrequencyAnalyzer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexFrequencyAnalyzer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneFrequencyAnalyzer);
            dolphin_deed(DolphinDeedSubGhzFrequencyAnalyzer);
            return true;
        } else if(event.event == SubmenuIndexFobAnalyzer) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexFobAnalyzer);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneFobAnalyzer);
            dolphin_deed(DolphinDeedSubGhzFrequencyAnalyzer);
            return true;
#ifndef PUBLIC_BUILD
        } else if(event.event == SubmenuIndexSpectrum) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSpectrum);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSpectrum);
            return true;
#endif
        } else if(event.event == SubmenuIndexExtSettings) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexExtSettings);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneExtModuleSettings);
            return true;
        }
#ifndef PUBLIC_BUILD
        else if(event.event == SubmenuIndexVehicleVulns) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexVehicleVulns);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneVulnMakes);
            return true;
        } else if(event.event == SubmenuIndexTpmsSpoof) {
            // TPMS Spoofer = listen for real TPMS sensors on 433.92 FSK, then spoof a
            // picked-up one (edit pressure/temp/ID + retransmit via the tpms_info view).
            // Reuses the receiver in a TPMS-only scan mode; NOT a from-scratch generator.
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexTpmsSpoof);
            subghz->tpms_scan_mode = true;
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        }
#endif
        else if(event.event == SubmenuIndexToolkit) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexToolkit);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneToolkit);
            return true;
        }
#ifndef PUBLIC_BUILD
        else if(event.event == SubmenuIndexRollBack) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexRollBack);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneRollbackCapture);
            return true;
        }
#endif
        else if(event.event == SubmenuIndexSignalDecoder) {
            scene_manager_set_scene_state(
                subghz->scene_manager, SubGhzSceneStart, SubmenuIndexSignalDecoder);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneProtoview);
            return true;
        }
    }
    return false;
}

void subghz_scene_start_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
