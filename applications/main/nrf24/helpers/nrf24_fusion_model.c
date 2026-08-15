#include "nrf24_fusion_model.h"
#include <string.h>

uint8_t nrf24_fuse_intensity(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm) {
    uint8_t v = occ;
    if(wifi_dbm != -128) { uint8_t p = nrf24_dbm_to_pct(wifi_dbm); if(p > v) v = p; }
    if(ble_dbm  != -128) { uint8_t p = nrf24_dbm_to_pct(ble_dbm);  if(p > v) v = p; }
    return v;
}

Nrf24SourceClass nrf24_classify(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm, uint8_t occ_threshold) {
    if(wifi_dbm != -128) return SRC_WIFI;
    if(ble_dbm  != -128) return SRC_BLE;
    if(occ >= occ_threshold) return SRC_HOPPER;
    return SRC_NONE;
}

void nrf24_fusion_model_reset(FusedSpectrum* m) {
    memset(m->occ, 0, sizeof(m->occ));
    memset(m->peak, 0, sizeof(m->peak));
    memset(m->klass, 0, sizeof(m->klass));
    for(int i = 0; i < NRF24_FUSION_CH; i++) { m->wifi_dbm[i] = -128; m->ble_dbm[i] = -128; }
    m->sweep_count = 0; m->wifi_epoch = 0; m->ble_epoch = 0;
}
