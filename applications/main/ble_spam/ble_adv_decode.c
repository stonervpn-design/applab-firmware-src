#include "ble_adv_decode.h"
#include <stddef.h>

#define BLE_AD_TYPE_FLAGS               0x01
#define BLE_AD_TYPE_UUID16_INCOMPLETE   0x02
#define BLE_AD_TYPE_UUID16_COMPLETE     0x03
#define BLE_AD_TYPE_UUID128_INCOMPLETE  0x06
#define BLE_AD_TYPE_UUID128_COMPLETE    0x07
#define BLE_AD_TYPE_TX_POWER            0x0A
#define BLE_AD_TYPE_MANUFACTURER        0xFF

void ble_adv_decode(const uint8_t* adv, uint8_t len, BleAdvInfo* out) {
    if(adv == NULL || out == NULL) return;

    uint16_t pos = 0;
    while(pos < len) {
        uint8_t ad_len = adv[pos];
        if(ad_len == 0) break; // zero-length structure: nothing more to parse

        // The structure occupies [pos] = ad_len byte, then ad_len bytes
        // ([type] + payload). If that runs past the end of the buffer,
        // the structure is truncated/malformed - stop without reading it.
        if((uint32_t)pos + 1 + (uint32_t)ad_len > (uint32_t)len) break;

        uint8_t type = adv[pos + 1];
        const uint8_t* payload = &adv[pos + 2];
        uint8_t payload_len = (uint8_t)(ad_len - 1); // ad_len includes the type byte

        switch(type) {
        case BLE_AD_TYPE_FLAGS:
            if(payload_len >= 1) {
                out->flags = payload[0];
                out->has_flags = true;
            }
            break;

        case BLE_AD_TYPE_UUID16_INCOMPLETE:
        case BLE_AD_TYPE_UUID16_COMPLETE: {
            uint8_t n = payload_len / 2;
            for(uint8_t i = 0; i < n && out->uuid16_count < BLE_ADV_MAX_U16; i++) {
                uint16_t uuid = (uint16_t)payload[i * 2] | ((uint16_t)payload[i * 2 + 1] << 8);
                out->uuid16[out->uuid16_count++] = uuid;
            }
            break;
        }

        case BLE_AD_TYPE_UUID128_INCOMPLETE:
        case BLE_AD_TYPE_UUID128_COMPLETE:
            if(payload_len >= 16) {
                for(uint8_t i = 0; i < 16; i++) out->uuid128[i] = payload[i];
                out->has_uuid128 = true;
            }
            break;

        case BLE_AD_TYPE_TX_POWER:
            if(payload_len >= 1) {
                out->tx_power = (int8_t)payload[0];
                out->has_tx_power = true;
            }
            break;

        case BLE_AD_TYPE_MANUFACTURER:
            if(payload_len >= 2) {
                out->mfr_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
                out->mfr_present = true;
                uint8_t data_len = (uint8_t)(payload_len - 2);
                if(data_len > BLE_ADV_MAX_MFR) data_len = BLE_ADV_MAX_MFR;
                for(uint8_t i = 0; i < data_len; i++) out->mfr_data[i] = payload[2 + i];
                out->mfr_data_len = data_len;
            }
            break;

        default:
            break;
        }

        pos = (uint16_t)(pos + 1 + ad_len);
    }
}
