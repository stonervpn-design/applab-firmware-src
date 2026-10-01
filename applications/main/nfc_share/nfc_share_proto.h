#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NSH_VERSION         2u
#define NSH_PKT_ANNOUNCE    1u
#define NSH_PKT_REQUEST     2u
#define NSH_PKT_DATA        3u
#define NFC_TP_HDR_POLL     0x00u
#define NFC_TP_HDR_PKT      0x01u
#define NSH_HEADER_LENGTH   3u
#define NSH_CRC_LENGTH      2u
#define NSH_FILENAME_LENGTH 36u
#define NSH_DATA_LENGTH     240u
#define NSH_CTRL_PL_LEN     56u                    /* name+size+md5 = 36+4+16 */
#define NSH_DATA_PL_LEN     (4u + NSH_DATA_LENGTH) /* 244 */
#define NSH_ANNOUNCE_FRAME_LEN (1u + NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN + NSH_CRC_LENGTH) /* 62 */
#define NSH_REQUEST_FRAME_LEN  NSH_ANNOUNCE_FRAME_LEN                                        /* 62 */
#define NSH_DATA_FRAME_LEN     (1u + NSH_HEADER_LENGTH + NSH_DATA_PL_LEN + NSH_CRC_LENGTH)   /* 250 */

uint16_t nfc_share_crc16(const uint8_t* d, size_t n);
size_t nfc_share_build_announce(
    uint8_t* out, uint8_t tx_id, const char* name, uint32_t size, const uint8_t md5[16]);
size_t nfc_share_build_request(uint8_t* out, uint8_t tx_id, uint32_t range_start, uint32_t range_end);
size_t nfc_share_build_data(
    uint8_t* out, uint8_t tx_id, uint32_t block, const uint8_t* data, uint32_t data_len);
bool nfc_share_parse_announce(
    const uint8_t* buf, uint8_t n, uint8_t* tx_id, char name[37], uint32_t* size, uint8_t md5[16]);
bool nfc_share_parse_request(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* range_start, uint32_t* range_end);
bool nfc_share_parse_data(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* block,
    const uint8_t** data, uint32_t* data_len);
void nfc_share_dedup_name(const char* base_leaf, unsigned suffix, char* out, size_t out_sz);
