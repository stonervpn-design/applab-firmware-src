#include "../subghz_i.h"
#include "../helpers/subghz_fob_buttons.h"
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#ifndef PUBLIC_BUILD
#include "../helpers/tpms_watch.h"
#endif

#include <lib/subghz/blocks/custom_btn.h>

#include "applications/main/subghz/helpers/subghz_txrx_i.h"
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/protocols/psa.h>
#include <lib/subghz/protocols/schrader_gg4.h>
#ifndef PUBLIC_BUILD
#include <lib/subghz/protocols/tpms_ford.h>
#include <lib/subghz/protocols/tpms_renault.h>
#include <lib/subghz/protocols/tpms_citroen.h>
#include <lib/subghz/protocols/tpms_pmv107j.h>
#include <lib/subghz/protocols/tpms_jeep.h>
#include <lib/subghz/protocols/tpms_hyundai_vdo.h>
#include <lib/subghz/protocols/tpms_abarth124.h>
#include <lib/subghz/protocols/tpms_toyota.h>
#include <lib/subghz/protocols/tpms_porsche.h>
#include <lib/subghz/protocols/tpms_bmw_g3.h>
#include <lib/subghz/protocols/tpms_honda.h>
#include <lib/subghz/protocols/tpms_bmw.h>
#include <lib/subghz/protocols/tpms_truck.h>
#include <lib/subghz/protocols/tpms_nissan.h>
#include <lib/subghz/protocols/tpms_jansite.h>
#include <lib/subghz/protocols/schrader_eg53ma4.h>
#include <lib/subghz/protocols/tpms_elantra2012.h>
#include <lib/subghz/protocols/tpms_ave.h>
#include <lib/subghz/protocols/tpms_renault_0435r.h>
#include <lib/subghz/protocols/tpms_kia.h>
#include <lib/subghz/protocols/tpms_trw_fsk.h>
#include <lib/subghz/protocols/tpms_smartire.h>
#include <lib/subghz/protocols/tpms_jansite_ty468.h>
#include <lib/subghz/protocols/tpms_jansite_ty588.h>
#include <lib/subghz/protocols/tpms_mercedes_benz.h>
#include <lib/subghz/protocols/tpms_airpuxem.h>
#include <lib/subghz/protocols/tpms_tyreguard400.h>
#include <lib/subghz/protocols/tpms_gm.h>
#include <lib/subghz/protocols/tpms_eezrv.h>
#include <lib/subghz/protocols/tpms_trw_ook.h>
#include <lib/subghz/protocols/tpms_schrader_motorcycle.h>
#include <lib/subghz/protocols/tpms_imars_t240.h>
#include <lib/subghz/protocols/tpms_sefis_m3.h>
#include <lib/subghz/protocols/tpms_schraeder.h>
#include <lib/subghz/protocols/schrader_smd3ma4.h>
#include <lib/subghz/protocols/schrader_nis315g3.h>
#endif
#include <string.h>

#define TAG "SubGhzSceneReceiverInfo"

void subghz_scene_receiver_info_callback(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;

    if((result == GuiButtonTypeCenter) && (type == InputTypePress)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStart);
    } else if((result == GuiButtonTypeCenter) && (type == InputTypeRelease)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStop);
    } else if((result == GuiButtonTypeRight) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoSave);
    } else if((result == GuiButtonTypeLeft) && (type == InputTypeShort)) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoAnalyze);
    }
}

// Returns true if the currently-loaded decoder is a rolling-code (dynamic)
// protocol — the only case that offers the read-only Rolling-Code Analyzer.
static bool subghz_scene_receiver_info_is_dynamic(SubGhz* subghz) {
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol) return false;
    return decoder->protocol->type == SubGhzProtocolTypeDynamic;
}

// PSA (Peugeot/Citroen) captures get a Brute-Force action instead of the generic
// Analyzer on the Left slot (the PSA rolling key is recoverable by search).
static bool subghz_scene_receiver_info_is_psa(SubGhz* subghz) {
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol || !decoder->protocol->name) return false;
    return strcmp(decoder->protocol->name, PSA_PROTOCOL_NAME) == 0;
}

static bool subghz_scene_receiver_info_update_parser(void* context) {
    SubGhz* subghz = context;

    if(subghz_txrx_load_decoder_by_name_protocol(
           subghz->txrx,
           subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen))) {
        // we are trying to deserialize without checking for errors, since it is assumed that we just received this chignal
        subghz_protocol_decoder_base_deserialize(
            subghz_txrx_get_decoder(subghz->txrx),
            subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen));

        SubGhzRadioPreset* preset =
            subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);

        //Edit TX power, if necessary.
        subghz_txrx_set_tx_power(preset->data, preset->data_size, subghz->tx_power);

        subghz_txrx_set_preset(
            subghz->txrx,
            furi_string_get_cstr(preset->name),
            preset->frequency,
            preset->data,
            preset->data_size);

        return true;
    }
    return false;
}

// TPMS view action callback — translate view-level actions into the same custom
// events the widget callback emits so subghz_scene_receiver_info_on_event can
// reuse its existing TX/Save handling.
static void
    subghz_scene_receiver_info_tpms_action(SubGhzViewTpmsInfoAction action, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    switch(action) {
    case SubGhzViewTpmsInfoActionTxStart:
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStart);
        break;
    case SubGhzViewTpmsInfoActionTxStop:
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoTxStop);
        break;
    case SubGhzViewTpmsInfoActionSave:
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneReceiverInfoSave);
        break;
    case SubGhzViewTpmsInfoActionEdit: {
        // Stash the selected field so the edit scene knows what to ask for
        SubGhzViewTpmsField field =
            subghz_view_tpms_info_get_selected_field(subghz->subghz_tpms_info);
        subghz->tpms_edit_field = field;
        if(field == SubGhzViewTpmsFieldPressure ||
           field == SubGhzViewTpmsFieldTemperature ||
           field == SubGhzViewTpmsFieldId) {
            view_dispatcher_send_custom_event(
                subghz->view_dispatcher,
                (uint32_t)(field == SubGhzViewTpmsFieldId ?
                               SubGhzCustomEventTpmsEditId :
                               (field == SubGhzViewTpmsFieldTemperature ?
                                    SubGhzCustomEventTpmsEditTemperature :
                                    SubGhzCustomEventTpmsEditPressure)));
        }
        break;
    }
    case SubGhzViewTpmsInfoActionToggleBattery:
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventTpmsToggleBattery);
        break;
    case SubGhzViewTpmsInfoActionTrack: {
#ifndef PUBLIC_BUILD
        // Add this sensor's ID to the TPMS watch list; the scanner then alerts
        // when this vehicle is heard again.
        TPMSBlockGeneric generic = {0};
        FlipperFormat* fff =
            subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
        if(fff && tpms_block_generic_deserialize(&generic, fff) == SubGhzProtocolStatusOk &&
           tpms_watch_add(generic.id)) {
            notification_message(subghz->notifications, &sequence_success);
        } else {
            notification_message(subghz->notifications, &sequence_error);
        }
#endif
        break;
    }
    case SubGhzViewTpmsInfoActionZeroPsiToggle:
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventTpmsZeroPsiToggle);
        break;
    }
}

// TRUE only for the TPMS protocols that have BOTH a working encoder and a 0-PSI
// packer below — i.e. the ones the flat-tire spoof can actually transmit. The
// auto-fire arm gate (subghz_scene_receiver.c) uses this so a TPMS frame that
// only decodes under a non-transmittable protocol never routes into a TX that
// would fail with "Error in protocol parameters".
bool subghz_scene_receiver_info_tpms_is_packable(const char* pname) {
    if(!pname) return false;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_GG4_NAME) == 0) return true;
#ifndef PUBLIC_BUILD
    if(strcmp(pname, TPMS_PROTOCOL_FORD_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_RENAULT_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_CITROEN_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_PMV107J_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_JEEP_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_HYUNDAI_VDO_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_ABARTH124_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_TOYOTA_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_PORSCHE_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_BMW_G3_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_HONDA_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_BMW_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_TRUCK_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_NISSAN_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_JANSITE_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_EG53MA4_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_ELANTRA2012_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_AVE_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_RENAULT_0435R_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_KIA_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_TRW_FSK_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SMARTIRE_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_JANSITE_TY468_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_JANSITE_TY588_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_MERCEDES_BENZ_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_AIRPUXEM_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_TYREGUARD400_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_GM_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_EEZRV_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_TRW_OOK_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_MOTO_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_IMARS_T240_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SEFIS_M3_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRAEDER_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_SMD3MA4_NAME) == 0) return true;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_NIS315G3_NAME) == 0) return true;
#endif
    return false;
}

// Re-encode the edited TPMS frame with the correct per-protocol packer (mirrors
// subghz_scene_tpms_edit_repack) so a modified 0-PSI value is actually transmitted.
static void subghz_scene_receiver_info_tpms_repack(const char* pname, TPMSBlockGeneric* g) {
    if(!pname) return;
    if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_GG4_NAME) == 0) {
        tpms_protocol_schrader_gg4_pack(g);
    }
#ifndef PUBLIC_BUILD
    else if(strcmp(pname, TPMS_PROTOCOL_FORD_NAME) == 0) {
        tpms_protocol_ford_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_RENAULT_NAME) == 0) {
        tpms_protocol_renault_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_CITROEN_NAME) == 0) {
        tpms_protocol_citroen_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_PMV107J_NAME) == 0) {
        tpms_protocol_pmv107j_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_JEEP_NAME) == 0) {
        tpms_protocol_jeep_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_HYUNDAI_VDO_NAME) == 0) {
        tpms_protocol_hyundai_vdo_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_ABARTH124_NAME) == 0) {
        tpms_protocol_abarth124_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_TOYOTA_NAME) == 0) {
        tpms_protocol_toyota_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_PORSCHE_NAME) == 0) {
        tpms_protocol_porsche_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_BMW_G3_NAME) == 0) {
        tpms_protocol_bmw_g3_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_HONDA_NAME) == 0) {
        tpms_protocol_honda_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_BMW_NAME) == 0) {
        tpms_protocol_bmw_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_TRUCK_NAME) == 0) {
        tpms_protocol_truck_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_NISSAN_NAME) == 0) {
        tpms_protocol_nissan_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_JANSITE_NAME) == 0) {
        tpms_protocol_jansite_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_EG53MA4_NAME) == 0) {
        tpms_protocol_schrader_eg53ma4_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_ELANTRA2012_NAME) == 0) {
        tpms_protocol_tpms_elantra2012_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_AVE_NAME) == 0) {
        tpms_protocol_tpms_ave_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_RENAULT_0435R_NAME) == 0) {
        tpms_protocol_tpms_renault_0435r_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_KIA_NAME) == 0) {
        tpms_protocol_tpms_kia_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_TRW_FSK_NAME) == 0) {
        tpms_protocol_tpms_trw_fsk_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SMARTIRE_NAME) == 0) {
        tpms_protocol_tpms_smartire_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_JANSITE_TY468_NAME) == 0) {
        tpms_protocol_tpms_jansite_ty468_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_JANSITE_TY588_NAME) == 0) {
        tpms_protocol_tpms_jansite_ty588_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_MERCEDES_BENZ_NAME) == 0) {
        tpms_protocol_tpms_mercedes_benz_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_AIRPUXEM_NAME) == 0) {
        tpms_protocol_tpms_airpuxem_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_TYREGUARD400_NAME) == 0) {
        tpms_protocol_tpms_tyreguard400_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_GM_NAME) == 0) {
        tpms_protocol_tpms_gm_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_EEZRV_NAME) == 0) {
        tpms_protocol_tpms_eezrv_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_TRW_OOK_NAME) == 0) {
        tpms_protocol_tpms_trw_ook_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_MOTO_NAME) == 0) {
        tpms_protocol_tpms_schrader_motorcycle_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_IMARS_T240_NAME) == 0) {
        tpms_protocol_tpms_imars_t240_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SEFIS_M3_NAME) == 0) {
        tpms_protocol_tpms_sefis_m3_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SCHRAEDER_NAME) == 0) {
        tpms_protocol_tpms_schraeder_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_SMD3MA4_NAME) == 0) {
        tpms_protocol_schrader_smd3ma4_pack(g);
    } else if(strcmp(pname, TPMS_PROTOCOL_SCHRADER_NIS315G3_NAME) == 0) {
        tpms_protocol_schrader_nis315g3_pack(g);
    }
#endif
}

// Build a 0-bar (flat-tire) frame from the picked-up sensor into a NEW throwaway
// FlipperFormat WITHOUT touching the history item — the original capture stays
// intact so it can still be Saved to a .sub for later replay. tx_start only needs
// the FFF during its deserialize (the encoder then yields from its own buffer), so
// the caller frees it right after. Returns NULL if the protocol is unpackable.
static FlipperFormat* subghz_scene_receiver_info_tpms_build_zero_psi(SubGhz* subghz) {
    TPMSBlockGeneric generic = {0};
    const char* pname =
        subghz_history_get_protocol_name(subghz->history, subghz->idx_menu_chosen);
    generic.protocol_name = pname;
    FlipperFormat* orig = subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen);
    if(!orig || tpms_block_generic_deserialize(&generic, orig) != SubGhzProtocolStatusOk) {
        return NULL;
    }
    generic.protocol_name = pname;
    generic.pressure = 0.0f;
    subghz_scene_receiver_info_tpms_repack(pname, &generic);

    FlipperFormat* fff = flipper_format_string_alloc();
    SubGhzRadioPreset preset = subghz_txrx_get_preset(subghz->txrx);
    if(tpms_block_generic_serialize(&generic, fff, &preset) != SubGhzProtocolStatusOk) {
        flipper_format_free(fff);
        return NULL;
    }
    // Burst length for the auto-cycle (endless_tx=false) 0-PSI fire: the encoders
    // read "Repeat" and yield that many frames (~1s) before returning to the scan —
    // enough for a real ECU to latch, and long enough to catch on a single SDR.
    // Ignored by the manual hold-Left path (endless_tx=true loops until released).
    uint32_t repeat = 100;
    flipper_format_write_uint32(fff, "Repeat", &repeat, 1);
    return fff;
}

// Toggle TPMS_NO_BATT → 0 (ok) → 1 (low) → TPMS_NO_BATT and persist into the
// history item. Since the Schrader payload has no known battery bit in the data
// word, only the .sub-file "Batt" field changes (data + CRC stay intact).
static void subghz_scene_receiver_info_tpms_toggle_battery(SubGhz* subghz) {
    TPMSBlockGeneric generic = {0};
    generic.protocol_name = subghz_history_get_protocol_name(
        subghz->history, subghz->idx_menu_chosen);
    FlipperFormat* fff = subghz_history_get_raw_data(
        subghz->history, subghz->idx_menu_chosen);
    if(!fff || tpms_block_generic_deserialize(&generic, fff) != SubGhzProtocolStatusOk) {
        return;
    }
    generic.protocol_name = subghz_history_get_protocol_name(
        subghz->history, subghz->idx_menu_chosen);

    if(generic.battery_low == TPMS_NO_BATT) {
        generic.battery_low = 0;
    } else if(generic.battery_low == 0) {
        generic.battery_low = 1;
    } else {
        generic.battery_low = TPMS_NO_BATT;
    }

    // No data/CRC change needed — the battery field lives only in the .sub
    // file representation, not in the over-the-air payload for Schrader GG4.
    subghz_history_replace_tpms_payload(
        subghz->history, subghz->idx_menu_chosen, &generic);
}

// If the decoded protocol is a TPMS sensor, route to the dedicated TPMS view
// instead of the generic widget rendering. Returns true if the view was switched.
static bool subghz_scene_receiver_info_try_tpms(SubGhz* subghz) {
    SubGhzProtocolDecoderBase* decoder = subghz_txrx_get_decoder(subghz->txrx);
    if(!decoder || !decoder->protocol) return false;
    if(decoder->protocol->type != SubGhzProtocolTypeTpms) return false;

    FuriString* frequency_str = furi_string_alloc();
    FuriString* modulation_str = furi_string_alloc();
    subghz_txrx_get_frequency_and_modulation(
        subghz->txrx, frequency_str, modulation_str, false);

    bool can_save = subghz_txrx_protocol_is_serializable(subghz->txrx);
    bool can_send = subghz_txrx_protocol_is_transmittable(subghz->txrx, false);

    subghz_view_tpms_info_set_callback(
        subghz->subghz_tpms_info, subghz_scene_receiver_info_tpms_action, subghz);

    subghz_view_tpms_info_update(
        subghz->subghz_tpms_info,
        subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen),
        furi_string_get_cstr(frequency_str),
        furi_string_get_cstr(modulation_str),
        can_send,
        can_save);

    furi_string_free(frequency_str);
    furi_string_free(modulation_str);

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdTpmsInfo);
    return true;
}

// True when the selected decoded signal is a fully-cloneable car fob that the
// multi-button emulate grid supports (has a Lock/Unlock table or a single-button
// synth) — used to offer the "Fob" dpad shortcut straight to the ready fob.
static bool subghz_scene_receiver_info_is_fob_cloneable(SubGhz* subghz) {
#ifndef PUBLIC_BUILD
    SubGhzProtocolDecoderBase* d = subghz_txrx_get_decoder(subghz->txrx);
    if(!d || !d->protocol || !d->protocol->name) return false;
    const char* p = d->protocol->name;
    return subghz_fob_protocol_supported(p) || subghz_fob_single_button_synth(p);
#else
    (void)subghz;
    return false;
#endif
}

void subghz_scene_receiver_info_draw_widget(SubGhz* subghz) {
    if(subghz_scene_receiver_info_update_parser(subghz)) {
        if(subghz_scene_receiver_info_try_tpms(subghz)) {
            return;
        }
        FuriString* frequency_str = furi_string_alloc();
        FuriString* modulation_str = furi_string_alloc();
        FuriString* text = furi_string_alloc();

        subghz_txrx_get_frequency_and_modulation(
            subghz->txrx, frequency_str, modulation_str, false);
        widget_add_string_element(
            subghz->widget,
            78,
            0,
            AlignLeft,
            AlignTop,
            FontSecondary,
            furi_string_get_cstr(frequency_str));

        widget_add_string_element(
            subghz->widget,
            113,
            0,
            AlignLeft,
            AlignTop,
            FontSecondary,
            furi_string_get_cstr(modulation_str));
        subghz_protocol_decoder_base_get_string(subghz_txrx_get_decoder(subghz->txrx), text);
        widget_add_string_multiline_element(
            subghz->widget, 0, 0, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));

        furi_string_free(frequency_str);
        furi_string_free(modulation_str);
        furi_string_free(text);

        if(subghz_txrx_protocol_is_serializable(subghz->txrx)) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeRight,
                "Save",
                subghz_scene_receiver_info_callback,
                subghz);
        }
        // Removed static check
        if(subghz_txrx_protocol_is_transmittable(subghz->txrx, false)) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeCenter,
                "Send",
                subghz_scene_receiver_info_callback,
                subghz);
        }
        // Left slot (Center=Send, Right=Save stay intact): any rolling-code capture
        // gets the Signal-ID hub ("ID"), which identifies the fob and — for a
        // supported car fob — offers both the rolling analyzer / PSA brute AND the
        // multi-button emulate grid as spokes (see subghz_scene_analyze.c). Keeping
        // the hub on the Left slot means those spokes stay reachable for every
        // supported (always-dynamic) protocol.
        if(subghz_scene_receiver_info_is_dynamic(subghz)) {
            // Fully-cloneable fobs get a one-key "Fob" shortcut (dpad = Left arrow on
            // Cardputer / tilt-Left on M5Stick) straight to the ready lock/unlock grid;
            // everything else keeps the Signal-ID hub ("ID").
            const char* left_label =
                subghz_scene_receiver_info_is_fob_cloneable(subghz) ? "Fob" : "ID";
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeLeft,
                left_label,
                subghz_scene_receiver_info_callback,
                subghz);
        }
    } else {
        // The item's stored protocol name was not found among the ACTIVE
        // receiver's decoder slots — typically because it was captured in the
        // other (Automotive <-> general) mode and the receiver has since been
        // rebuilt from the other registry. Degrade gracefully instead of the
        // hard "Error history parse." wipe: keep the user on the info screen
        // showing whatever is stored (name + key data, frequency, preset) plus
        // a hint to re-open it after toggling the mode.
        FuriString* item_str = furi_string_alloc();
        FuriString* text = furi_string_alloc();
        subghz_history_get_text_item_menu(subghz->history, item_str, subghz->idx_menu_chosen);
        uint32_t frequency =
            subghz_history_get_frequency(subghz->history, subghz->idx_menu_chosen);
        const char* preset = subghz_history_get_preset(subghz->history, subghz->idx_menu_chosen);
        furi_string_printf(
            text,
            "%s\n%lu.%02lu MHz %s\n\nNot in current mode -\ntoggle mode to re-open.",
            furi_string_get_cstr(item_str),
            (unsigned long)(frequency / 1000000UL),
            (unsigned long)((frequency % 1000000UL) / 10000UL),
            preset ? preset : "");
        widget_add_string_multiline_element(
            subghz->widget, 0, 0, AlignLeft, AlignTop, FontSecondary, furi_string_get_cstr(text));
        furi_string_free(text);
        furi_string_free(item_str);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

void subghz_scene_receiver_info_on_enter(void* context) {
    SubGhz* subghz = context;

    subghz_custom_btns_reset();
    subghz->tpms_zero_psi_firing = false;

    subghz_scene_receiver_info_draw_widget(subghz);

    if(!subghz_history_get_text_space_left(subghz->history, NULL) &&
       !scene_manager_has_previous_scene(subghz->scene_manager, SubGhzSceneDecodeRAW)) {
        subghz->state_notifications = SubGhzNotificationStateRx;
    }

    // Auto-fire 0-PSI: arrived from the spoofer scan's hands-off attack. Queue the
    // 0-PSI toggle so on_event starts the flat-tire spoof now that the (TPMS) view
    // is up. No-op for non-transmittable sensors (the toggle handler falls back).
    if(subghz->tpms_auto_fire_on_open) {
        subghz->tpms_auto_fire_on_open = false;
        subghz->tpms_auto_cycle = true; // burst then return to the scan (capture->fire loop)
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventTpmsZeroPsiToggle);
    }
}

bool subghz_scene_receiver_info_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneReceiverInfoTxStart) {
            if(!subghz_scene_receiver_info_update_parser(subghz)) {
                return false;
            }
            //CC1101 Stop RX -> Start TX
            subghz_txrx_hopper_pause(subghz->txrx);
            // key concept: we start endless TX until user release OK button, and after this we send last
            // protocols repeats - this guarantee that one press OK will
            // be guarantee send the required minimum protocol data packets
            // for all of this we use subghz_block_generic_global.endless_tx in protocols _yield function.
            subghz->state_notifications = SubGhzNotificationStateTx;
            subghz_block_generic_global.endless_tx = true;
            if(!subghz_tx_start(
                   subghz,
                   subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen))) {
                subghz_txrx_rx_start(subghz->txrx);
                subghz_txrx_hopper_unpause(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
                subghz_block_generic_global.endless_tx = false;
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoTxStop) {
            //CC1101 Stop Tx -> next tick event Start RX
            // user release OK
            // we switch off endless_tx - that mean protocols yield finish endless transmission,
            // send upload "repeat=xx" times, and after will be stoped by the tick event down in this code
            subghz->state_notifications = SubGhzNotificationStateTxWait;
            subghz_block_generic_global.endless_tx = false;

            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoSave) {
            // If the 0-PSI spoof is firing, stop it cleanly first so we can save the
            // ORIGINAL captured sensor (the history item was never modified).
            if(subghz->tpms_zero_psi_firing) {
                subghz_block_generic_global.endless_tx = false;
                subghz->tpms_zero_psi_firing = false;
                subghz_view_tpms_info_set_zero_psi_firing(subghz->subghz_tpms_info, false);
            }
            //CC1101 Stop RX -> Save
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);

            subghz_txrx_stop(subghz->txrx);
            if(!subghz_scene_receiver_info_update_parser(subghz)) {
                return false;
            }

            if(subghz_txrx_protocol_is_serializable(subghz->txrx)) {
                subghz_file_name_clear(subghz);

                subghz->save_datetime =
                    subghz_history_get_datetime(subghz->history, subghz->idx_menu_chosen);
                subghz->save_datetime_set = true;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaveName);
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneReceiverInfoAnalyze) {
#ifndef PUBLIC_BUILD
            const bool _fob_quick = subghz_scene_receiver_info_is_fob_cloneable(subghz);
#endif
            // Route to the Signal-ID hub; it identifies the fob (make/model/action)
            // and offers the deeper rolling / PSA tool as a "Details" spoke.
            // Stop RX first (mirrors the Save/TpmsEdit spokes) so the ~8K lazy RX worker is
            // freed before the Analyze spokes (PSA brute / rollback replay) spawn their own
            // internal worker stack — otherwise that alloc OOMs -> reboot on this maxed board.
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
            subghz_txrx_stop(subghz->txrx);
#ifndef PUBLIC_BUILD
            // dpad "Fob" shortcut: for a fully-cloneable car fob, skip the Signal-ID
            // hub and jump straight to the ready lock/unlock emulate grid.
            if(_fob_quick) {
                SubGhzRadioPreset* rp =
                    subghz_history_get_radio_preset(subghz->history, subghz->idx_menu_chosen);
                if(rp) {
                    subghz_txrx_set_preset(
                        subghz->txrx,
                        furi_string_get_cstr(rp->name),
                        rp->frequency,
                        rp->data,
                        rp->data_size);
                }
                Stream* dst =
                    flipper_format_get_raw_stream(subghz_txrx_get_fff_data(subghz->txrx));
                Stream* src = flipper_format_get_raw_stream(
                    subghz_history_get_raw_data(subghz->history, subghz->idx_menu_chosen));
                stream_clean(dst);
                stream_rewind(src);
                stream_copy_full(src, dst);
                subghz->keeloq_fob_from_garage = true;
                subghz->keeloq_fob_generic = true;
                scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
                return true;
            }
#endif
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneAnalyze);
            return true;
        } else if(
            event.event == SubGhzCustomEventTpmsEditPressure ||
            event.event == SubGhzCustomEventTpmsEditTemperature ||
            event.event == SubGhzCustomEventTpmsEditId) {
            // Stop RX while the edit scene is active so the underlying decoder
            // stays stable, then hand off.
            subghz->state_notifications = SubGhzNotificationStateIDLE;
            subghz_txrx_hopper_set_state(subghz->txrx, SubGhzHopperStateOFF);
            subghz_txrx_stop(subghz->txrx);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTpmsEdit);
            return true;
        } else if(event.event == SubGhzCustomEventTpmsToggleBattery) {
            subghz_scene_receiver_info_tpms_toggle_battery(subghz);
            // Re-load the TPMS view from the now-modified history payload
            subghz_scene_receiver_info_update_parser(subghz);
            subghz_scene_receiver_info_try_tpms(subghz);
            return true;
        } else if(event.event == SubGhzCustomEventTpmsZeroPsiToggle) {
            if(!subghz->tpms_zero_psi_firing) {
                // START: build a 0-bar flat-tire frame from the capture WITHOUT
                // modifying the history item (so it stays saveable via Save), then
                // transmit it. Auto-cycle mode fires a finite BURST (endless_tx=false
                // -> N repeats) and the TxWait tick returns to the scan to capture the
                // next sensor; the manual hold-Left path fires endlessly until toggled
                // off. The screen shows the real captured values behind the banner.
                FlipperFormat* zfff = subghz_scene_receiver_info_tpms_build_zero_psi(subghz);
                if(!zfff) {
                    // Nothing to fire -> in auto-cycle, go back to capturing.
                    if(subghz->tpms_auto_cycle) {
                        subghz->tpms_auto_cycle = false;
                        scene_manager_search_and_switch_to_previous_scene(
                            subghz->scene_manager, SubGhzSceneReceiver);
                    } else {
                        notification_message(subghz->notifications, &sequence_error);
                    }
                    return true;
                }
                subghz_txrx_hopper_pause(subghz->txrx);
                subghz_block_generic_global.endless_tx = subghz->tpms_auto_cycle ? false : true;
                if(subghz_tx_start(subghz, zfff)) {
                    subghz->tpms_zero_psi_firing = true;
                    subghz_view_tpms_info_set_zero_psi_firing(subghz->subghz_tpms_info, true);
#ifndef PUBLIC_BUILD
                    if(subghz->tpms_auto_cycle) {
                        // Auto-fired: tick this sensor's list row and stop re-firing /
                        // re-listing it for the rest of this scan session.
                        subghz_history_set_fired(
                            subghz->history, subghz->idx_menu_chosen, true);
                        subghz_tpms_fired_add(subghz, subghz->tpms_auto_fire_sensor_id);
                    }
#endif
                    // Auto-cycle: TxWait watches for the burst to finish, then returns
                    // to the scan. Manual: stay firing (Tx) until the user stops.
                    subghz->state_notifications = subghz->tpms_auto_cycle ?
                                                      SubGhzNotificationStateTxWait :
                                                      SubGhzNotificationStateTx;
                } else {
                    // TX failed -> in auto-cycle go back to capturing, else fall to RX.
                    subghz_block_generic_global.endless_tx = false;
                    if(subghz->tpms_auto_cycle) {
                        subghz->tpms_auto_cycle = false;
                        flipper_format_free(zfff);
                        scene_manager_search_and_switch_to_previous_scene(
                            subghz->scene_manager, SubGhzSceneReceiver);
                        return true;
                    }
                    subghz_txrx_rx_start(subghz->txrx);
                    subghz_txrx_hopper_unpause(subghz->txrx);
                    subghz->state_notifications = SubGhzNotificationStateRx;
                    notification_message(subghz->notifications, &sequence_error);
                }
                flipper_format_free(zfff);
            } else {
                // STOP: end endless TX; the TxWait tick path stops TX + restarts RX.
                subghz_block_generic_global.endless_tx = false;
                subghz->tpms_zero_psi_firing = false;
                subghz_view_tpms_info_set_zero_psi_firing(subghz->subghz_tpms_info, false);
                subghz->state_notifications = SubGhzNotificationStateTxWait;
            }
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(subghz_txrx_hopper_get_state(subghz->txrx) != SubGhzHopperStateOFF) {
            subghz_txrx_hopper_update(subghz->txrx, subghz->last_settings->hopping_threshold);
        }
        switch(subghz->state_notifications) {
        case SubGhzNotificationStateTx:
            notification_message(subghz->notifications, &sequence_blink_magenta_10);
            break;
        case SubGhzNotificationStateRx:
            notification_message(subghz->notifications, &sequence_blink_cyan_10);
            break;
        case SubGhzNotificationStateRxDone:
            notification_message(subghz->notifications, &sequence_blink_green_100);
            subghz->state_notifications = SubGhzNotificationStateRx;
            break;
        case SubGhzNotificationStateTxWait:
            // we wait until hardware TX finished and after stop TX and start RX, else just blink led
            if(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device)) {
                notification_message(subghz->notifications, &sequence_blink_magenta_10);
            } else {
                subghz_txrx_stop(subghz->txrx);

                // Auto-cycle: the 0-PSI burst finished -> clear firing and go BACK to
                // the scan to capture the next sensor (capture->fire->capture loop,
                // until the user leaves the scan).
                if(subghz->tpms_auto_cycle) {
                    subghz->tpms_auto_cycle = false;
                    subghz->tpms_zero_psi_firing = false;
                    subghz_view_tpms_info_set_zero_psi_firing(subghz->subghz_tpms_info, false);
                    subghz->state_notifications = SubGhzNotificationStateIDLE;
                    scene_manager_search_and_switch_to_previous_scene(
                        subghz->scene_manager, SubGhzSceneReceiver);
                    break;
                }

                // update screen
                widget_reset(subghz->widget);
                subghz_scene_receiver_info_draw_widget(subghz);

                subghz->state_notifications = SubGhzNotificationStateIDLE;

                if(!scene_manager_has_previous_scene(subghz->scene_manager, SubGhzSceneDecodeRAW)) {
                    subghz_txrx_rx_start(subghz->txrx);
                    subghz_txrx_hopper_unpause(subghz->txrx);
                    if(!subghz_history_get_text_space_left(subghz->history, NULL)) {
                        subghz->state_notifications = SubGhzNotificationStateRx;
                    }
                }
            }
            break;
        default:
            break;
        }
    }
    return false;
}

void subghz_scene_receiver_info_on_exit(void* context) {
    SubGhz* subghz = context;

    // Stop the continuous 0-PSI spoof if the user left the screen while it fired.
    if(subghz->tpms_zero_psi_firing) {
        subghz_block_generic_global.endless_tx = false;
        // Let the in-flight async frame finish before tearing down, so the radio is
        // left idle-clean; a hard stop mid-frame could leave the async TX engine busy
        // and make the NEXT auto-fire's tx_start fail on a return-to-scan re-attack.
        uint32_t guard = 0;
        while(!subghz_devices_is_async_complete_tx(subghz->txrx->radio_device) &&
              guard++ < 200) {
            furi_delay_ms(1);
        }
        subghz_txrx_stop(subghz->txrx);
        subghz->tpms_zero_psi_firing = false;
        subghz_view_tpms_info_set_zero_psi_firing(subghz->subghz_tpms_info, false);
    }

    // Belt-and-suspenders: clear the auto-fire arm/route flags on the way out so a
    // return to the SAME scan session always re-arms cleanly (the scan on_enter also
    // resets these, but do it here too in case of any resume path).
    subghz->tpms_auto_fire_pending = false;
    subghz->tpms_auto_fire_on_open = false;
    subghz->tpms_auto_cycle = false;

    widget_reset(subghz->widget);
    subghz_txrx_reset_dynamic_and_custom_btns(subghz->txrx);
}
