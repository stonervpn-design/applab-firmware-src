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
#ifndef PUBLIC_BUILD
    &subghz_protocol_genie,
#endif
    &tpms_protocol_schrader_gg4, &tpms_protocol_ford,
    &tpms_protocol_renault,      &tpms_protocol_citroen,
    &tpms_protocol_pmv107j,
#ifndef PUBLIC_BUILD
    &tpms_protocol_hyundai_vdo,
#endif
};

const SubGhzProtocolRegistry subghz_protocol_registry = {
    .items = subghz_protocol_registry_items,
    .size = COUNT_OF(subghz_protocol_registry_items)};

// --- Automotive protocol set — ProtoPirate car decoders, one resident set. ---
// Measured resident footprint of all 61 decoders is ~20-25 KB (largest single is
// hitag2 ~1.8 KB; most carry no capture buffer) against ~103 KB free — the general
// set already holds ~66 decoders. The former Cars 1 / Cars 2 split was based on a
// stale "won't fit" assumption inherited from the heavyweight app decoders and is
// no longer needed.
static const SubGhzProtocol* const subghz_protocol_registry_items_automotive[] = {
#ifndef PUBLIC_BUILD
    &subghz_protocol_raw,        &subghz_protocol_bin_raw,
    // former group A
    &bmw_cas4_protocol,          &hyundai_kia_rio_protocol,
    &hyundai_tl_protocol,        &mazda_siemens_protocol,
    &sheriff_cfm_protocol,       &fiat_v1_protocol,
    &chrysler_protocol_v0,       &fiat_protocol_v0,
    &ford_protocol_v0,           &ford_protocol_v1,
    &ford_protocol_v2,           &ford_protocol_v3,
    &honda_static_protocol,      &kia_protocol_v0,
    &kia_protocol_v1,            &kia_protocol_v2,
    &kia_protocol_v3_v4,         &kia_protocol_v5,
    &kia_protocol_v6,            &kia_protocol_v7,
    &land_rover_v0_protocol,     &land_rover_rke_protocol,
    &mazda_v0_protocol,
    &mitsubishi_v0_protocol,     &porsche_touareg_protocol,
    &psa_protocol,               &subghz_protocol_scher_khan,
    &subghz_protocol_star_line,  &subaru_protocol,
    &vag_protocol,               &subghz_protocol_audi_v1,
    &subghz_protocol_audi_v2,
    // former group B
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
    &subghz_protocol_peugot_v0,      &subghz_protocol_renault_v1,
    &subghz_protocol_renault_v2,     &subghz_protocol_renault_v3,
    &subghz_protocol_renault_v4,     &subghz_protocol_toyota_v0,
    &toyota_keeloq_protocol,
#else
    &subghz_protocol_raw,
    &subghz_protocol_bin_raw,
#endif
};

static const SubGhzProtocolRegistry subghz_protocol_registry_automotive = {
    .items = subghz_protocol_registry_items_automotive,
    .size = COUNT_OF(subghz_protocol_registry_items_automotive)};

// Car mode: 0 = general (Off), 1 = Cars (single merged automotive set).
static uint8_t s_subghz_car_mode = 0;

uint8_t subghz_protocol_registry_car_mode(void) {
    return s_subghz_car_mode;
}

void subghz_protocol_registry_set_car_mode(uint8_t mode) {
    // Any nonzero (incl. a stale saved "Cars 2" == 2) maps to the single Cars mode.
    s_subghz_car_mode = (mode >= 1) ? 1 : 0;
}

bool subghz_protocol_registry_automotive_mode(void) {
    return s_subghz_car_mode != 0;
}

const SubGhzProtocolRegistry* subghz_protocol_registry_active(void) {
    return (s_subghz_car_mode == 1) ? &subghz_protocol_registry_automotive
                                     : &subghz_protocol_registry;
}
