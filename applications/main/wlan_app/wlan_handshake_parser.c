#include "wlan_handshake_parser.h"
#include <string.h>

bool wlan_hs_is_beacon(const uint8_t* payload, int len) {
    if(len < 24) return false;
    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;
    return (frame_type == 0 && frame_subtype == 8);
}

bool wlan_hs_parse_addresses(
    const uint8_t* payload, int len,
    const uint8_t** bssid, const uint8_t** station, const uint8_t** ap,
    int* header_len) {
    if(len < 24) return false;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_subtype = (fc & 0xF0) >> 4;
    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;

    *header_len = 24;
    if(to_ds && from_ds) *header_len = 30;
    if((frame_subtype & 0x08) == 0x08) *header_len += 2; // QoS

    if(len < *header_len + 8) return false;

    const uint8_t* addr1 = &payload[4];
    const uint8_t* addr2 = &payload[10];
    const uint8_t* addr3 = &payload[16];

    if(!to_ds && from_ds) {
        *bssid = addr2; *ap = addr2; *station = addr1;
    } else if(to_ds && !from_ds) {
        *bssid = addr1; *ap = addr1; *station = addr2;
    } else if(!to_ds && !from_ds) {
        *bssid = addr3; *station = addr2; *ap = addr1;
    } else {
        *bssid = addr1; *station = addr2; *ap = addr1;
    }
    return true;
}

bool wlan_hs_is_eapol(const uint8_t* payload, int header_len, int len) {
    if(len < header_len + 8) return false;
    const uint8_t* llc = &payload[header_len];
    return (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
            llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
            llc[6] == 0x88 && llc[7] == 0x8E);
}

uint8_t wlan_hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len) {
    if(eapol_len < 4) return 0;
    uint8_t packet_type = eapol_start[1];
    if(packet_type != 0x03) return 0;
    if(eapol_len < 95) return 0;

    uint16_t key_info = (eapol_start[5] << 8) | eapol_start[6];
    bool key_mic = (key_info & 0x0100) != 0;
    bool secure = (key_info & 0x0200) != 0;
    bool key_ack = (key_info & 0x0080) != 0;
    bool install = (key_info & 0x0040) != 0;
    bool pairwise = (key_info & 0x0008) != 0;

    if(pairwise && key_ack && !install && !key_mic) return 1;
    if(pairwise && !key_ack && !install && key_mic && !secure) return 2;
    if(pairwise && key_ack && install && key_mic) return 3;
    if(pairwise && !key_ack && !install && key_mic && secure) return 4;
    return 0;
}

/* Extract the RSN PMKID from an EAPOL-Key M1 frame (clientless WPA capture).
 * Key Data starts at eapol_start[99]; the PMKID KDE is DD 14 00 0F AC 04 <16 bytes>.
 * Returns true and fills pmkid_out[16] if a non-zero PMKID is present. */
bool wlan_hs_extract_pmkid(const uint8_t* eapol_start, int eapol_len, uint8_t pmkid_out[16]) {
    if(eapol_len < 99) return false;
    int kd_len = (eapol_start[97] << 8) | eapol_start[98];
    if(99 + kd_len > eapol_len) kd_len = eapol_len - 99;
    const uint8_t* kd = &eapol_start[99];
    for(int i = 0; i + 22 <= kd_len; i++) {
        if(kd[i] == 0xDD && kd[i + 1] == 0x14 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0F &&
           kd[i + 4] == 0xAC && kd[i + 5] == 0x04) {
            const uint8_t* p = &kd[i + 6];
            bool nonzero = false;
            for(int j = 0; j < 16; j++)
                if(p[j]) {
                    nonzero = true;
                    break;
                }
            if(!nonzero) return false; // some APs advertise a zeroed PMKID
            memcpy(pmkid_out, p, 16);
            return true;
        }
    }
    return false;
}

bool wlan_hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len) {
    if(len < 38) {
        ssid_out[0] = '\0';
        return false;
    }
    int pos = 36;
    while(pos + 2 <= len) {
        uint8_t tag_id = payload[pos];
        uint8_t tag_len = payload[pos + 1];
        if(pos + 2 + tag_len > len) break;
        if(tag_id == 0) { // SSID
            int copy_len = tag_len;
            if(copy_len >= max_len) copy_len = max_len - 1;
            memcpy(ssid_out, &payload[pos + 2], copy_len);
            ssid_out[copy_len] = '\0';
            return copy_len > 0;
        }
        pos += 2 + tag_len;
    }
    ssid_out[0] = '\0';
    return false;
}
