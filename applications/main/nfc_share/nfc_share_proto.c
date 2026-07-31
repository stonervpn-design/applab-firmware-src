#include "nfc_share_proto.h"
#include <string.h>
#include <stdio.h>

uint16_t nfc_share_crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for(int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* append LE crc16 over p[0..body_len) at p[body_len]; returns 1+body_len+2 frame len */
static size_t finish_packet(uint8_t* out, size_t body_len) {
    uint8_t* p = out + 1; /* out[0] = tp_hdr already set */
    uint16_t crc = nfc_share_crc16(p, body_len);
    p[body_len] = (uint8_t)(crc & 0xFF);
    p[body_len + 1] = (uint8_t)(crc >> 8);
    return 1u + body_len + NSH_CRC_LENGTH;
}

size_t nfc_share_build_announce(
    uint8_t* out, uint8_t tx_id, const char* name, uint32_t size, const uint8_t md5[16]) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_ANNOUNCE;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memset(pl, 0, NSH_CTRL_PL_LEN);
    size_t nlen = strlen(name);
    if(nlen > NSH_FILENAME_LENGTH - 1) nlen = NSH_FILENAME_LENGTH - 1;
    memcpy(pl, name, nlen);
    memcpy(pl + NSH_FILENAME_LENGTH, &size, 4);
    memcpy(pl + NSH_FILENAME_LENGTH + 4, md5, 16);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
}

size_t nfc_share_build_request(uint8_t* out, uint8_t tx_id, uint32_t rs, uint32_t re) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_REQUEST;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memset(pl, 0, NSH_CTRL_PL_LEN);
    memcpy(pl, &rs, 4);
    memcpy(pl + 4, &re, 4);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
}

size_t nfc_share_build_data(
    uint8_t* out, uint8_t tx_id, uint32_t block, const uint8_t* data, uint32_t data_len) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_DATA;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memcpy(pl, &block, 4);
    if(data_len > NSH_DATA_LENGTH) data_len = NSH_DATA_LENGTH;
    memcpy(pl + 4, data, data_len);
    if(data_len < NSH_DATA_LENGTH) memset(pl + 4 + data_len, 0, NSH_DATA_LENGTH - data_len);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_DATA_PL_LEN);
}

/* Shared parse preamble: verify tp_hdr, version, type, and crc over body_len.
 * buf/n include tp_hdr + trailing transport CRC-A (which we ignore). Returns the
 * nsh packet pointer or NULL. */
static const uint8_t* parse_common(const uint8_t* buf, uint8_t n, uint8_t type, size_t body_len) {
    if(!buf || buf[0] != NFC_TP_HDR_PKT) return NULL;
    if(n < 1u + body_len + NSH_CRC_LENGTH) return NULL;
    const uint8_t* nsh = buf + 1;
    if(nsh[0] != NSH_VERSION || nsh[2] != type) return NULL;
    uint16_t got = (uint16_t)(nsh[body_len] | ((uint16_t)nsh[body_len + 1] << 8));
    if(nfc_share_crc16(nsh, body_len) != got) return NULL;
    return nsh;
}

bool nfc_share_parse_announce(
    const uint8_t* buf, uint8_t n, uint8_t* tx_id, char name[37], uint32_t* size, uint8_t md5[16]) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_ANNOUNCE, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
    if(!nsh) return false;
    const uint8_t* pl = nsh + NSH_HEADER_LENGTH;
    if(tx_id) *tx_id = nsh[1];
    memset(name, 0, 37);
    memcpy(name, pl, NSH_FILENAME_LENGTH); /* NUL-padded in wire form */
    name[NSH_FILENAME_LENGTH] = '\0';
    memcpy(size, pl + NSH_FILENAME_LENGTH, 4);
    memcpy(md5, pl + NSH_FILENAME_LENGTH + 4, 16);
    return true;
}

bool nfc_share_parse_request(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* rs, uint32_t* re) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_REQUEST, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
    if(!nsh || nsh[1] != exp_tx_id) return false;
    memcpy(rs, nsh + NSH_HEADER_LENGTH, 4);
    memcpy(re, nsh + NSH_HEADER_LENGTH + 4, 4);
    return true;
}

bool nfc_share_parse_data(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* block,
    const uint8_t** data, uint32_t* data_len) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_DATA, NSH_HEADER_LENGTH + NSH_DATA_PL_LEN);
    if(!nsh || nsh[1] != exp_tx_id) return false;
    memcpy(block, nsh + NSH_HEADER_LENGTH, 4);
    *data = nsh + NSH_HEADER_LENGTH + 4;
    *data_len = NSH_DATA_LENGTH;
    return true;
}

void nfc_share_dedup_name(const char* base_leaf, unsigned suffix, char* out, size_t out_sz) {
    if(suffix == 0) {
        snprintf(out, out_sz, "%s", base_leaf);
        return;
    }
    const char* dot = strrchr(base_leaf, '.');
    if(dot && dot != base_leaf) {
        int stem = (int)(dot - base_leaf);
        snprintf(out, out_sz, "%.*s_%u%s", stem, base_leaf, suffix, dot);
    } else {
        snprintf(out, out_sz, "%s_%u", base_leaf, suffix);
    }
}
