#ifndef PUBLIC_BUILD

#include "wlan_mgmt_frames.h"
#include <string.h>

static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Supported rates tag payload: 1, 2, 5.5, 11 Mbit/s (basic, non-HT). */
static const uint8_t kRates[4] = {0x02, 0x04, 0x0B, 0x16};

uint16_t wlan_build_probe_req(uint8_t* out, uint16_t cap, const uint8_t src[6], const char* ssid) {
    if(!out || !src) return 0;
    size_t ssid_len = ssid ? strnlen(ssid, 32) : 0;
    uint16_t total = (uint16_t)(24 + (2 + ssid_len) + (2 + sizeof(kRates)));
    if(cap < total) return 0;

    uint16_t i = 0;
    out[i++] = 0x40; /* frame control: mgmt, subtype Probe Request */
    out[i++] = 0x00;
    out[i++] = 0x00; /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], kBroadcast, 6); i += 6; /* addr1 = broadcast RA */
    memcpy(&out[i], src, 6); i += 6; /* addr2 = SA */
    memcpy(&out[i], kBroadcast, 6); i += 6; /* addr3 = wildcard BSSID */
    out[i++] = 0x00; /* seq control */
    out[i++] = 0x00;
    /* SSID tag */
    out[i++] = 0x00;
    out[i++] = (uint8_t)ssid_len;
    if(ssid_len) {
        memcpy(&out[i], ssid, ssid_len);
        i += (uint16_t)ssid_len;
    }
    /* Supported rates tag */
    out[i++] = 0x01;
    out[i++] = (uint8_t)sizeof(kRates);
    memcpy(&out[i], kRates, sizeof(kRates));
    i += sizeof(kRates);
    return i;
}

uint16_t
    wlan_build_beacon(uint8_t* out, uint16_t cap, const uint8_t bssid[6], const char* ssid, uint8_t channel) {
    if(!out || !bssid) return 0;
    size_t ssid_len = ssid ? strnlen(ssid, 32) : 0;
    uint16_t total = (uint16_t)(24 + 12 + (2 + ssid_len) + (2 + sizeof(kRates)) + 3);
    if(cap < total) return 0;

    uint16_t i = 0;
    out[i++] = 0x80; /* frame control: mgmt, subtype Beacon */
    out[i++] = 0x00;
    out[i++] = 0x00; /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], kBroadcast, 6); i += 6; /* addr1 = broadcast RA */
    memcpy(&out[i], bssid, 6); i += 6; /* addr2 = BSSID */
    memcpy(&out[i], bssid, 6); i += 6; /* addr3 = BSSID */
    out[i++] = 0x00; /* seq control */
    out[i++] = 0x00;
    /* fixed params */
    memset(&out[i], 0, 8); i += 8; /* timestamp */
    out[i++] = 0x64; /* beacon interval: 100 TU */
    out[i++] = 0x00;
    out[i++] = 0x01; /* capability info: ESS + Privacy */
    out[i++] = 0x04;
    /* SSID tag */
    out[i++] = 0x00;
    out[i++] = (uint8_t)ssid_len;
    if(ssid_len) {
        memcpy(&out[i], ssid, ssid_len);
        i += (uint16_t)ssid_len;
    }
    /* Supported rates tag */
    out[i++] = 0x01;
    out[i++] = (uint8_t)sizeof(kRates);
    memcpy(&out[i], kRates, sizeof(kRates));
    i += sizeof(kRates);
    /* DS Parameter Set tag */
    out[i++] = 0x03;
    out[i++] = 0x01;
    out[i++] = channel;
    return i;
}

uint16_t wlan_build_deauth(
    uint8_t* out,
    uint16_t cap,
    const uint8_t bssid[6],
    const uint8_t sta[6],
    uint16_t reason) {
    if(!out || !bssid || !sta) return 0;
    const uint16_t total = 26;
    if(cap < total) return 0;

    uint16_t i = 0;
    out[i++] = 0xC0; /* frame control: mgmt, subtype Deauthentication */
    out[i++] = 0x00;
    out[i++] = 0x00; /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], sta, 6); i += 6; /* addr1 = RA = target STA */
    memcpy(&out[i], bssid, 6); i += 6; /* addr2 = TA = BSSID */
    memcpy(&out[i], bssid, 6); i += 6; /* addr3 = BSSID */
    out[i++] = 0x00; /* seq control */
    out[i++] = 0x00;
    out[i++] = (uint8_t)(reason & 0xFF); /* reason code, little-endian */
    out[i++] = (uint8_t)((reason >> 8) & 0xFF);
    return i; /* 26 */
}

#endif /* PUBLIC_BUILD */
