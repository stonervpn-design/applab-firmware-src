/* Shared ESB promiscuous-frame decode -- see nrf24_esb.h.
 *
 * PRIVATE-only: whole body #ifndef PUBLIC_BUILD (only the private attack
 * scenes call it, so it contributes zero symbols to the public build). This
 * matches nrf24_capture_tx.c's explicit-guard precedent rather than relying on
 * --gc-sections alone. */
#ifndef PUBLIC_BUILD

#include "nrf24_esb.h"

#include <string.h>

/* CRC16-CCITT (poly 0x1021), MSB-first, `bits` bits of `byte`. */
static uint16_t esb_crc_update(uint16_t crc, uint8_t byte, uint8_t bits) {
    crc = crc ^ ((uint16_t)byte << 8);
    while(bits--) {
        if((crc & 0x8000) == 0x8000) {
            crc = (uint16_t)((crc << 1) ^ 0x1021);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc & 0xFFFF;
}

bool nrf24_esb_decode_frame(
    const uint8_t* raw_buf,
    uint8_t size,
    uint8_t* out_addr, /* 5 bytes */
    uint8_t* out_payload, /* 32 bytes */
    uint8_t* out_len) {
    if(size < 10) return false;
    if(size > 37) size = 37;

    uint8_t buf[37];
    /* Try both 0xAA and 0x55 preamble alignments, same as mj_fingerprint(). */
    for(int offset = 0; offset < 2; offset++) {
        memcpy(buf, raw_buf, size);
        if(offset == 1) {
            for(int x = size - 1; x >= 0; x--) {
                if(x > 0) {
                    buf[x] = (uint8_t)((buf[x - 1] << 7) | (buf[x] >> 1));
                } else {
                    buf[x] = (uint8_t)(buf[x] >> 1);
                }
            }
        }

        uint8_t payload_length = buf[5] >> 2;
        if(payload_length == 0 || payload_length > 32) continue;
        if(payload_length > (uint8_t)(size - 9)) continue;

        uint16_t crc_given = ((uint16_t)buf[6 + payload_length] << 9) |
                             ((uint16_t)buf[7 + payload_length] << 1);
        crc_given = (uint16_t)((crc_given << 8) | (crc_given >> 8));
        if(buf[8 + payload_length] & 0x80) crc_given |= 0x0100;

        uint16_t crc_calc = 0xFFFF;
        for(int x = 0; x < 6 + payload_length; x++) {
            crc_calc = esb_crc_update(crc_calc, buf[x], 8);
        }
        crc_calc = esb_crc_update(crc_calc, buf[6 + payload_length] & 0x80, 1);
        crc_calc = (uint16_t)((crc_calc << 8) | (crc_calc >> 8));

        if(crc_calc != crc_given) continue;

        memcpy(out_addr, buf, 5);
        for(int x = 0; x < payload_length; x++) {
            out_payload[x] = (uint8_t)(((buf[6 + x] << 1) & 0xFF) | (buf[7 + x] >> 7));
        }
        *out_len = payload_length;
        return true;
    }
    return false;
}

#endif /* !PUBLIC_BUILD */
