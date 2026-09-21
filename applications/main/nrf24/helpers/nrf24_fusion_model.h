#pragma once
#include <stdint.h>
#include "nrf24_chanmap.h"

typedef enum { SRC_NONE = 0, SRC_HOPPER = 1, SRC_WIFI = 2, SRC_BLE = 3 } Nrf24SourceClass;

typedef struct {
    uint8_t  occ[NRF24_FUSION_CH];
    uint8_t  peak[NRF24_FUSION_CH];
    int8_t   wifi_dbm[NRF24_FUSION_CH];
    int8_t   ble_dbm[NRF24_FUSION_CH];
    uint8_t  klass[NRF24_FUSION_CH];   /* Nrf24SourceClass */
    uint16_t sweep_count;
    uint32_t wifi_epoch;
    uint32_t ble_epoch;
} FusedSpectrum;

uint8_t nrf24_fuse_intensity(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm);
Nrf24SourceClass nrf24_classify(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm, uint8_t occ_threshold);
void nrf24_fusion_model_reset(FusedSpectrum* m);
