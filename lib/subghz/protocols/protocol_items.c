#include "protocol_items.h"

// --- General protocol set (DEFAULT) — native gate/garage/TPMS/etc. ---
// The ProtoPirate automotive decoders are intentionally NOT here: this board
// (ESP32-S3FN8, NO PSRAM, ~103 KB free) cannot hold them resident alongside
// the general set. They live in the automotive registry below and load only
// when the user enables Automotive mode (receiver is rebuilt on toggle).
const SubGhzProtocol* const subghz_protocol_registry_items[] = {
    &subghz_protocol_gate_tx,       &subghz_protocol_keeloq,
    &subghz_protocol_nice_flo,      &subghz_protocol_came,
    &subghz_protocol_faac_slh,      &subghz_protocol_nice_flor_s,
    &subghz_protocol_came_twee,     &subghz_protocol_came_atomo,
    &subghz_protocol_nero_sketch,   &subghz_protocol_ido,
    &subghz_protocol_hormann,       &subghz_protocol_nero_radio,
    &subghz_protocol_somfy_telis,   &subghz_protocol_somfy_keytis,
    &subghz_protocol_princeton,     &subghz_protocol_raw,
    &subghz_protocol_linear,        &subghz_protocol_secplus_v2,
    &subghz_protocol_secplus_v1,    &subghz_protocol_megacode,
    &subghz_protocol_holtek,        &subghz_protocol_chamb_code,
    &subghz_protocol_power_smart,   &subghz_protocol_marantec,
    &subghz_protocol_bett,          &subghz_protocol_doitrand,
    &subghz_protocol_phoenix_v2,    &subghz_protocol_honeywell_wdb,
    &subghz_protocol_magellan,      &subghz_protocol_intertechno_v3,
    &subghz_protocol_clemsa,        &subghz_protocol_ansonic,
    &subghz_protocol_smc5326,       &subghz_protocol_holtek_th12x,
    &subghz_protocol_linear_delta3, &subghz_protocol_dooya,
    &subghz_protocol_alutech_at_4n, &subghz_protocol_kinggates_stylo_4k,
    &subghz_protocol_bin_raw,       &subghz_protocol_mastercode,
    &subghz_protocol_honeywell,     &subghz_protocol_legrand,
    &subghz_protocol_dickert_mahs,  &subghz_protocol_gangqi,
    &subghz_protocol_marantec24,    &subghz_protocol_hollarm,
    &subghz_protocol_hay21,         &subghz_protocol_revers_rb2,
    &subghz_protocol_feron,         &subghz_protocol_roger,
    &subghz_protocol_elplast,       &subghz_protocol_treadmill37,
    &subghz_protocol_beninca_arc,   &subghz_protocol_jarolift,
    &subghz_protocol_ditec_gol4,    &subghz_protocol_keyfinder,
    &subghz_protocol_telcoma_edge,  &subghz_protocol_allstar_firefly,
    &subghz_protocol_nord_ice,      &subghz_protocol_x10,
    &subghz_protocol_hormann_bisecur, &subghz_protocol_cardin_s508,
    &subghz_protocol_pocsag,
    &subghz_protocol_genie,
    &tpms_protocol_schrader_gg4, &tpms_protocol_ford,
    &tpms_protocol_renault,      &tpms_protocol_citroen,
    &tpms_protocol_pmv107j,
    &tpms_protocol_hyundai_vdo,
};

const SubGhzProtocolRegistry subghz_protocol_registry = {
    .items = subghz_protocol_registry_items,
    .size = COUNT_OF(subghz_protocol_registry_items)};

// --- Automotive protocol set — ProtoPirate car decoders, loaded on-demand ---
// Includes raw/bin_raw so undecoded car signals can still be captured. 26
// decoders total; the working baseline was ~60 native, so this fits with room.
const SubGhzProtocol* const subghz_protocol_registry_items_automotive[] = {
    &subghz_protocol_raw,        &subghz_protocol_bin_raw,
#ifndef PUBLIC_BUILD  // car-key decoders below are compiled out of public builds (see root CMakeLists.txt FLIPPER_VARIANT=public)
    &bmw_cas4_protocol,          &hyundai_kia_rio_protocol,
    &mazda_siemens_protocol,     &sheriff_cfm_protocol,
    &fiat_v1_protocol,           &chrysler_protocol_v0,
    &fiat_protocol_v0,           &ford_protocol_v0,
    &ford_protocol_v1,           &ford_protocol_v2,
    &ford_protocol_v3,           &honda_static_protocol,
    &kia_protocol_v0,            &kia_protocol_v1,
    &kia_protocol_v2,            &kia_protocol_v3_v4,
    &kia_protocol_v5,            &kia_protocol_v6,
    &kia_protocol_v7,            &land_rover_v0_protocol,
    &mazda_v0_protocol,          &mitsubishi_v0_protocol,
    &porsche_touareg_protocol,   &psa_protocol,
    &subghz_protocol_scher_khan, &subghz_protocol_star_line,
    &subaru_protocol,            &vag_protocol,
    &vag_polo_protocol,          &santa_fe_protocol,
    // --- Archiv WIP automotive batch (added 2026-07-24) ---
    &subghz_protocol_audi_v1,        &subghz_protocol_audi_v2,
    &subghz_protocol_audi_v3,        &subghz_protocol_bmw_v2,
    &subghz_protocol_bmw_v3,         &subghz_protocol_chevrolet_v0,
    &subghz_protocol_ducato_v0,      &subghz_protocol_fiat_spa,
    &subghz_protocol_fiat_v2_v3_v5,  &subghz_protocol_hitag2,
    &subghz_protocol_honda_v2_v3,    &subghz_protocol_honda_v4,
    &subghz_protocol_honda_v5,       &subghz_protocol_honda_v6,
    &subghz_protocol_honda_v7,       &subghz_protocol_honda_v8,
    &subghz_protocol_honda_v9,       &subghz_protocol_honda_v10,
    &subghz_protocol_jeep_v0,        &subghz_protocol_ldv_v0,
    &subghz_protocol_lexus_v0,       &subghz_protocol_mazda_v1,
    &subghz_protocol_mazda_v2,       &subghz_protocol_mitsubishi_v1,
    &subghz_protocol_nissan_v0,      &subghz_protocol_opel_v0,
    &subghz_protocol_peugot_v0,
    &subghz_protocol_renault_v1,     &subghz_protocol_renault_v2,
    &subghz_protocol_renault_v3,     &subghz_protocol_renault_v4,
    &subghz_protocol_toyota_v0,
    // Ported rtl_433 car remotes + TPMS + specials (propagated from momentum-tembed)
    &subghz_protocol_compustar,      &subghz_protocol_code_alarm,
    &subghz_protocol_audiovox,       &subghz_protocol_astrostart,
    &subghz_protocol_continental_rke, &subghz_protocol_gm_rke,
    &subghz_protocol_nidec_rke,      &subghz_protocol_alps_rke,
    &subghz_protocol_siemens_vdo,    &subghz_protocol_mic6sc2,
    &tpms_protocol_toyota,           &tpms_protocol_trw,
    &tpms_protocol_honda,            &tpms_protocol_jeep,
    &tpms_protocol_nissan,           &tpms_protocol_kia,
    &tpms_protocol_gm,               &tpms_protocol_bmw,
    &tpms_protocol_bmw_g3,           &tpms_protocol_mercedes,
    &tpms_protocol_porsche,          &tpms_protocol_abarth124,
    &tpms_protocol_elantra2012,      &tpms_protocol_renault_0435r,
    &tpms_protocol_jansite,          &tpms_protocol_jansite_solar,
    &tpms_protocol_jansite_ty468,    &tpms_protocol_jansite_ty588,
    &tpms_protocol_eezrv,            &tpms_protocol_smartire,
    &tpms_protocol_tyreguard400,     &tpms_protocol_imars_t240,
    &tpms_protocol_ave,              &tpms_protocol_airpuxem,
    &tpms_protocol_gear_hive,        &tpms_protocol_sefis_m3,
    &tpms_protocol_truck,            &tpms_protocol_schrader_moto,
    &subghz_protocol_tesla_charge,   &subghz_protocol_mercedes_fob,
    &subghz_protocol_porsche_cayenne,
#endif // PUBLIC_BUILD: car-key (RKE/immobiliser) decoders stripped for the public applab build;
       // raw/bin_raw above stay so the Automotive toggle + every reference still link (no car decoders exposed).
};

const SubGhzProtocolRegistry subghz_protocol_registry_automotive = {
    .items = subghz_protocol_registry_items_automotive,
    .size = COUNT_OF(subghz_protocol_registry_items_automotive)};

// --- On-demand mode selector (default: general) ---
static bool s_subghz_automotive_mode = false;

bool subghz_protocol_registry_automotive_mode(void) {
    return s_subghz_automotive_mode;
}

void subghz_protocol_registry_set_automotive_mode(bool enabled) {
    s_subghz_automotive_mode = enabled;
}

const SubGhzProtocolRegistry* subghz_protocol_registry_active(void) {
    return s_subghz_automotive_mode ? &subghz_protocol_registry_automotive :
                                      &subghz_protocol_registry;
}
