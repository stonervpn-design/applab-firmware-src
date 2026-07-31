#include "../nfc_share_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 1. CRC-16/CCITT-FALSE known vector: "123456789" -> 0x29B1 */
    assert(nfc_share_crc16((const uint8_t*)"123456789", 9) == 0x29B1);

    /* 2. ANNOUNCE build -> parse round trip */
    uint8_t md5[16];
    for(int i = 0; i < 16; i++) md5[i] = (uint8_t)(i * 7 + 1);
    uint8_t f[256];
    size_t fl = nfc_share_build_announce(f, 0x5A, "hello.txt", 1234, md5);
    assert(fl == 62);
    /* emulate the RX side: transport CRC-A adds 2 bytes to the frame */
    uint8_t rx[256];
    memcpy(rx, f, fl);
    rx[fl] = 0xAB; rx[fl + 1] = 0xCD;
    uint8_t txid = 0; char name[37]; uint32_t size = 0; uint8_t gmd5[16];
    assert(nfc_share_parse_announce(rx, (uint8_t)(fl + 2), &txid, name, &size, gmd5));
    assert(txid == 0x5A && size == 1234 && strcmp(name, "hello.txt") == 0);
    assert(memcmp(gmd5, md5, 16) == 0);

    /* 3. REQUEST build -> parse (56-byte control payload, length keyed by type) */
    size_t rl = nfc_share_build_request(f, 0x5A, 240, 1234);
    assert(rl == 62);
    memcpy(rx, f, rl); rx[rl] = 0x11; rx[rl + 1] = 0x22;
    uint32_t rs = 0, re = 0;
    assert(nfc_share_parse_request(rx, (uint8_t)(rl + 2), 0x5A, &rs, &re));
    assert(rs == 240 && re == 1234);
    assert(!nfc_share_parse_request(rx, (uint8_t)(rl + 2), 0x99, &rs, &re)); /* wrong txid */

    /* 4. DATA build -> parse */
    uint8_t payload[240];
    for(int i = 0; i < 240; i++) payload[i] = (uint8_t)(255 - i);
    size_t dl = nfc_share_build_data(f, 0x5A, 3, payload, 240);
    assert(dl == 250);
    memcpy(rx, f, dl); rx[dl] = 0; rx[dl + 1] = 0;
    uint32_t blk = 0; const uint8_t* dp = NULL; uint32_t dn = 0;
    assert(nfc_share_parse_data(rx, (uint8_t)(dl + 2), 0x5A, &blk, &dp, &dn));
    assert(blk == 3 && dn == 240 && memcmp(dp, payload, 240) == 0);

    /* 5. DATA short tail is zero-padded to 240 */
    dl = nfc_share_build_data(f, 0x5A, 0, payload, 5);
    assert(dl == 250);
    assert(f[1 + 3 + 4 + 5] == 0 && f[1 + 3 + 4 + 239] == 0);

    /* 6. dedup name */
    char out[64];
    nfc_share_dedup_name("m5share.txt", 0, out, sizeof(out));
    assert(strcmp(out, "m5share.txt") == 0);
    nfc_share_dedup_name("m5share.txt", 2, out, sizeof(out));
    assert(strcmp(out, "m5share_2.txt") == 0);
    nfc_share_dedup_name("noext", 1, out, sizeof(out));
    assert(strcmp(out, "noext_1") == 0);

    printf("nfc_share_proto: ALL PASS\n");
    return 0;
}
