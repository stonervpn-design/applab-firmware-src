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
    &subghz_protocol_retekess,
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
#ifndef PUBLIC_BUILD
    &subghz_protocol_genie,
#endif
    &tpms_protocol_schrader_gg4, &tpms_protocol_ford,
    &tpms_protocol_renault,      &tpms_protocol_citroen,
    &tpms_protocol_pmv107j,
    &tpms_protocol_toyota,       &tpms_protocol_honda,
    &tpms_protocol_nissan,
    &tesla_chargeport_protocol,
#ifndef PUBLIC_BUILD
    &tpms_protocol_hyundai_vdo,
    &tpms_protocol_hyundai_ev,
#endif
};

const SubGhzProtocolRegistry subghz_protocol_registry = {
    .items = subghz_protocol_registry_items,
    .size = COUNT_OF(subghz_protocol_registry_items)};

// --- Automotive protocol set — ProtoPirate car decoders, one resident set. ---
// Measured resident footprint of all ~75 decoders is ~20-25 KB (largest single is
// hitag2 ~1.8 KB; most carry no capture buffer) against ~103 KB free — the general
// set already holds ~66 decoders. The former Cars 1 / Cars 2 split was based on a
// stale "won't fit" assumption inherited from the heavyweight app decoders and is
// no longer needed.
static const SubGhzProtocol* const subghz_protocol_registry_items_automotive[] = {
#ifndef PUBLIC_BUILD
    &subghz_protocol_raw,        &subghz_protocol_bin_raw,
    // former group A
    &bmw_cas4_protocol,          &hyundai_kia_rio_protocol,
    &hyundai_tl_protocol,        &santa_fe_protocol, // Hyundai Santa Fe — was defined but unregistered (dead); now wired
    &mazda_siemens_protocol,
    &sheriff_cfm_protocol,       &fiat_v1_protocol,
    &chrysler_protocol_v0,       &fiat_protocol_v0,
    &ford_protocol_v0,           &ford_protocol_v1,
    &ford_protocol_v2,           &ford_protocol_v3,
    &honda_static_protocol,      &kia_protocol_v0,
    &kia_protocol_v1,            &kia_protocol_v2,
    &kia_protocol_v3_v4,         &kia_protocol_v5,
    &kia_protocol_v6,            &kia_protocol_v7,
    &land_rover_v0_protocol,     &land_rover_rke_protocol,
    &volvo_rke_protocol,
    &bmw_ook_protocol,
    &mazda_v0_protocol,
    &mitsubishi_v0_protocol,     &porsche_touareg_protocol,
    &subghz_protocol_porsche_cayenne,
    &psa_protocol,               &subghz_protocol_scher_khan,
    &subghz_protocol_star_line,  &subaru_protocol,
    &vag_protocol,               &subghz_protocol_audi_v1,
    &subghz_protocol_audi_v2,
    // former group B
    &subghz_protocol_audi_v3,        &subghz_protocol_bmw_v2,
    &subghz_protocol_bmw_v3,         &subghz_protocol_chevrolet_v0,
    &subghz_protocol_ducato_v0,      &subghz_protocol_fiat_spa,
    &subghz_protocol_fiat_v2_v3_v5,  &fiat_v2_protocol,
    &subghz_protocol_hitag2,
    &subghz_protocol_honda_v2_v3,    &subghz_protocol_honda_v4,
    &subghz_protocol_honda_v5,       &subghz_protocol_honda_v6,
    &subghz_protocol_honda_v7,       &subghz_protocol_honda_v8,
    &subghz_protocol_honda_v9,       &subghz_protocol_honda_v10,
    &subghz_protocol_jeep_v0,        &subghz_protocol_ldv_v0,
    &subghz_protocol_lexus_v0,       &subghz_protocol_mazda_v1,
    &subghz_protocol_mazda_v2,       &subghz_protocol_mitsubishi_v1,
    &subghz_protocol_nissan_v0,      &subghz_protocol_opel_v0,
    &renault_v0_protocol,
    &subghz_protocol_peugot_v0,      &subghz_protocol_renault_v1,
    &subghz_protocol_renault_v2,     &subghz_protocol_renault_v3,
    &subghz_protocol_renault_v4,     &subghz_protocol_toyota_v0,
    &toyota_keeloq_protocol,
    &subghz_protocol_suzuki,         &subghz_protocol_fiat_marelli,
    // Aftermarket remote-start + OEM RKE (decode-only; grafted from tembed-mob)
    &subghz_protocol_compustar,      &subghz_protocol_astrostart,
    &subghz_protocol_audiovox,       &subghz_protocol_gm_rke,
    &subghz_protocol_code_alarm,     &subghz_protocol_six_sc_two,
    &subghz_protocol_continental_rke, &subghz_protocol_nidec_rke,
    &subghz_protocol_alps_rke,       &vag_polo_protocol,
    &subghz_protocol_genie, // in automotive too so the Genie Recorder decodes from Car Keys
#else
    &subghz_protocol_raw,
    &subghz_protocol_bin_raw,
#endif
};

__attribute__((unused)) static const SubGhzProtocolRegistry subghz_protocol_registry_automotive = {
    .items = subghz_protocol_registry_items_automotive,
    .size = COUNT_OF(subghz_protocol_registry_items_automotive)};

#ifndef PUBLIC_BUILD
// --- Sensors set — passive rtl_433 weather stations + extended TPMS (decode-only). ---
// RAM-gated: loaded ONLY in Sensors mode so the always-resident general set stays
// small on no-PSRAM boards. Includes RAW + all TPMS so it is a full scanning set.
static const SubGhzProtocol* const subghz_protocol_registry_items_sensors[] = {
    &subghz_protocol_raw,             &subghz_protocol_bin_raw,
    &subghz_protocol_acurite_tower,   &subghz_protocol_lacrosse_tx141th,
    &subghz_protocol_lacrosse_tx,     &subghz_protocol_nexus,
    &subghz_protocol_prologue,        &subghz_protocol_oregon_thgr,
    &subghz_protocol_fineoffset_wh2,  &subghz_protocol_ambient_weather,
    &tpms_protocol_bmw,               &tpms_protocol_bmw_g3,
    &tpms_protocol_jansite,           &tpms_protocol_truck,
    &tpms_protocol_jeep,              &tpms_protocol_abarth124,
    &tpms_protocol_porsche,
    &tpms_protocol_schrader_gg4,      &tpms_protocol_ford,
    &tpms_protocol_renault,           &tpms_protocol_citroen,
    &tpms_protocol_pmv107j,           &tpms_protocol_toyota,
    &tpms_protocol_honda,             &tpms_protocol_nissan,
    &tpms_protocol_hyundai_vdo,
    &tpms_protocol_hyundai_ev,
    // Extended rtl_433 TPMS ports (2026-09-07)
    &tpms_protocol_tpms_elantra2012,  &tpms_protocol_tpms_ave,
    &tpms_protocol_tpms_renault_0435r,&tpms_protocol_tpms_kia,
    &tpms_protocol_tpms_trw_fsk,      &tpms_protocol_tpms_smartire,
    &tpms_protocol_tpms_jansite_ty468,&tpms_protocol_tpms_jansite_ty588,
    &tpms_protocol_tpms_mercedes_benz,&tpms_protocol_tpms_airpuxem,
    &tpms_protocol_tpms_tyreguard400,
    &tpms_protocol_tpms_gm,           &tpms_protocol_tpms_eezrv,
    &tpms_protocol_tpms_trw_ook,      &tpms_protocol_tpms_schrader_motorcycle,
    &tpms_protocol_tpms_imars_t240,   &tpms_protocol_tpms_sefis_m3,
    &tpms_protocol_tpms_schraeder,    &tpms_protocol_schrader_smd3ma4,
    &tpms_protocol_schrader_nis315g3,
};
static const SubGhzProtocolRegistry subghz_protocol_registry_sensors = {
    .items = subghz_protocol_registry_items_sensors,
    .size = COUNT_OF(subghz_protocol_registry_items_sensors)};
#endif

// Car mode: 0 = general (Off), 1 = Cars (single merged automotive set).
static uint8_t s_subghz_car_mode = 0;

uint8_t subghz_protocol_registry_car_mode(void) {
    return s_subghz_car_mode;
}

void subghz_protocol_registry_set_car_mode(uint8_t mode) {
    // 0 = general (Off), 1 = Cars (automotive set), 2 = Sensors (weather+TPMS set).
    s_subghz_car_mode = (mode <= 2) ? mode : 0;
}

bool subghz_protocol_registry_automotive_mode(void) {
    return s_subghz_car_mode != 0;
}

const SubGhzProtocolRegistry* subghz_protocol_registry_active(void) {
#ifndef PUBLIC_BUILD
    if(s_subghz_car_mode == 1) return &subghz_protocol_registry_automotive;
    if(s_subghz_car_mode == 2) return &subghz_protocol_registry_sensors;
#endif
    return &subghz_protocol_registry;
}
