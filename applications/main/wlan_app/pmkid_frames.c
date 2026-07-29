#include "pmkid_frames.h"
#include <string.h>

/* 8 default supported rates: 1, 2, 5.5, 11, 18, 24, 36, 54 Mbit/s. */
static const uint8_t kRates[8] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};

/* RSN IE payload (after tag id 0x30 + length 0x14): version, group cipher CCMP,
 * 1 pairwise CCMP, 1 AKM PSK, RSN capabilities 0x0000. */
static const uint8_t kRsn[20] = {
    0x01, 0x00,                   /* version */
    0x00, 0x0F, 0xAC, 0x04,       /* group cipher: CCMP */
    0x01, 0x00,                   /* pairwise count */
    0x00, 0x0F, 0xAC, 0x04,       /* pairwise: CCMP */
    0x01, 0x00,                   /* AKM count */
    0x00, 0x0F, 0xAC, 0x02,       /* AKM: PSK */
    0x00, 0x00,                   /* RSN capabilities */
};

size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]) {
    if(cap < 30) return 0;
    size_t i = 0;
    out[i++] = 0xB0;              /* frame control: mgmt, subtype Authentication */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], bssid, 6); i += 6;   /* addr1 = BSSID */
    memcpy(&out[i], src, 6);   i += 6;   /* addr2 = SA */
    memcpy(&out[i], bssid, 6); i += 6;   /* addr3 = BSSID */
    out[i++] = 0x00;             /* seq control */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* auth algorithm: open system */
    out[i++] = 0x00;
    out[i++] = 0x01;             /* auth transaction seq = 1 */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* status code = 0 */
    out[i++] = 0x00;
    return i;                    /* 30 */
}

size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid) {
    size_t ssid_len = ssid ? strnlen(ssid, 32) : 0;
    size_t total = 24 + 4 + (2 + ssid_len) + (2 + sizeof(kRates)) + (2 + sizeof(kRsn));
    if(cap < total) return 0;

    size_t i = 0;
    out[i++] = 0x00;             /* frame control: mgmt, subtype Assoc Request */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], bssid, 6); i += 6;   /* addr1 = BSSID */
    memcpy(&out[i], src, 6);   i += 6;   /* addr2 = SA */
    memcpy(&out[i], bssid, 6); i += 6;   /* addr3 = BSSID */
    out[i++] = 0x00;             /* seq control */
    out[i++] = 0x00;
    /* fixed params */
    out[i++] = 0x11;             /* capability info: ESS + Privacy */
    out[i++] = 0x00;
    out[i++] = 0x0A;             /* listen interval */
    out[i++] = 0x00;
    /* SSID tag */
    out[i++] = 0x00;
    out[i++] = (uint8_t)ssid_len;
    if(ssid_len) { memcpy(&out[i], ssid, ssid_len); i += ssid_len; }
    /* Supported rates tag */
    out[i++] = 0x01;
    out[i++] = (uint8_t)sizeof(kRates);
    memcpy(&out[i], kRates, sizeof(kRates)); i += sizeof(kRates);
    /* RSN IE */
    out[i++] = 0x30;
    out[i++] = (uint8_t)sizeof(kRsn);
    memcpy(&out[i], kRsn, sizeof(kRsn)); i += sizeof(kRsn);
    return i;
}
