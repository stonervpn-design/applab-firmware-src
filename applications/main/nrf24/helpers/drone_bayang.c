/* Bayang toy-quadcopter nRF24 packet builder -- see drone_bayang.h for the
 * full source citation and UNVERIFIED status.
 *
 * PRIVATE-only: whole body #ifndef PUBLIC_BUILD (only the Task-3 private
 * bind/control scene calls this, so it contributes zero symbols to the
 * public build). Matches nrf24_esb.c's explicit-guard precedent rather
 * than relying on --gc-sections alone. */
#ifndef PUBLIC_BUILD

#include "drone_bayang.h"

#include <string.h>

/* Clamp one axis value into the documented [0, BAYANG_AXIS_MAX] protocol
 * range before packing, so absurd inputs (e.g. 0xFFFF) stay within the
 * field's documented 10-bit range. NOTE: buffer/neighbor-byte safety does
 * NOT depend on this clamp -- the packers write each field with fixed byte
 * indices and per-byte masks (out[k]=(v>>8)&0xFF; out[k+1]=v&0xFF), so no
 * input can write outside its own two byte slots regardless of value. This
 * clamp only enforces the protocol range, not memory safety. */
static uint16_t bayang_clamp_axis(uint16_t v) {
    return (v > BAYANG_AXIS_MAX) ? (uint16_t)BAYANG_AXIS_MAX : v;
}

uint8_t drone_bayang_checksum(const uint8_t* pkt, uint8_t len) {
    /* NULL pkt or len==0: nothing to sum, return the bare seed byte. Never
     * dereferences pkt in this branch. */
    if(!pkt || len == 0) return 0x55u;

    uint16_t sum = 0;
    uint8_t n = (uint8_t)(len - 1); /* checksum trails at pkt[len-1], sums pkt[0..len-2] */
    for(uint8_t i = 0; i < n; i++) sum += pkt[i];
    return (uint8_t)((sum + 0x55u) & 0xFFu);
}

void drone_bayang_rf_channels(const uint8_t txid[3], uint8_t out_ch[BAYANG_RF_NUM_CH]) {
    if(!out_ch) return;

    uint8_t t0 = 0, t1 = 0, t2 = 0;
    if(txid) {
        t0 = txid[0];
        t1 = txid[1];
        t2 = txid[2];
    }

    /* UNVERIFIED: this weighted-sum-mod-126 derivation is NOT the real
     * source's channel-hop algorithm (that formula was not confidently
     * recalled at implementation time) -- it only structurally guarantees
     * the documented FACT that all 4 channels are txid-derived, spread out,
     * deterministic, and <= 125 (the nRF24 2.4GHz channel range). Needs
     * replacing with the exact bayang.c/Bayang_nrf24l01.ino hop formula
     * once that source is available to transcribe byte-exactly. */
    for(uint8_t i = 0; i < BAYANG_RF_NUM_CH; i++) {
        uint32_t v = (uint32_t)t0 * 79u + (uint32_t)t1 * 37u + (uint32_t)t2 * 17u +
                      (uint32_t)i * 23u + 1u;
        out_ch[i] = (uint8_t)(v % 126u); /* 0..125 inclusive */
    }
}

void drone_bayang_build_bind(const uint8_t txid[3], uint8_t out[BAYANG_PKT_LEN]) {
    if(!out) return;
    memset(out, 0, BAYANG_PKT_LEN);

    out[0] = (uint8_t)BAYANG_PKT_BIND; /* 0xA4: bind command byte */

    /* [1..5]: 5-byte bind address. UNVERIFIED: the plan Reference cites a
     * "{0x00,0x00,0x00,0x00,0x00}-style" documented bind address; using
     * that literal all-zero placeholder here rather than inventing a
     * plausible-looking non-zero constant. Real bind address needs
     * confirming against the source/a capture. */
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x00;

    /* [6..9]: the 4 RF hop channels (see drone_bayang_rf_channels). */
    uint8_t ch[BAYANG_RF_NUM_CH];
    drone_bayang_rf_channels(txid, ch);
    out[6] = ch[0];
    out[7] = ch[1];
    out[8] = ch[2];
    out[9] = ch[3];

    out[10] = 0x00; /* reserved */

    /* [11..13]: 3-byte txid, same position as in control packets. */
    uint8_t tx0 = 0, tx1 = 0, tx2 = 0;
    if(txid) {
        tx0 = txid[0];
        tx1 = txid[1];
        tx2 = txid[2];
    }
    out[11] = tx0;
    out[12] = tx1;
    out[13] = tx2;

    out[14] = drone_bayang_checksum(out, BAYANG_PKT_LEN); /* [14]: checksum */
}

void drone_bayang_build_control(
    const uint8_t txid[3],
    uint16_t throttle,
    uint16_t yaw,
    uint16_t pitch,
    uint16_t roll,
    uint8_t flags,
    uint8_t out[BAYANG_PKT_LEN]) {
    if(!out) return;
    memset(out, 0, BAYANG_PKT_LEN);

    uint16_t t = bayang_clamp_axis(throttle);
    uint16_t y = bayang_clamp_axis(yaw);
    uint16_t p = bayang_clamp_axis(pitch);
    uint16_t r = bayang_clamp_axis(roll);

    out[0] = (uint8_t)BAYANG_PKT_CONTROL; /* 0xA5: control/data command byte */
    out[1] = flags; /* BAYANG_FLAG_* bits */

    /* [2..9]: throttle/yaw/pitch/roll, each a big-endian 16-bit field
     * (documented range packed into the low 10 bits; clamped above so the
     * high 6 bits are always 0). UNVERIFIED: the exact byte offsets for
     * each axis are transcribed from the plan Reference's general
     * description ("big-endian fields ... typically 0..0x3FF") rather
     * than a byte-exact re-read of bayang.c at implementation time. */
    out[2] = (uint8_t)((t >> 8) & 0xFFu);
    out[3] = (uint8_t)(t & 0xFFu);
    out[4] = (uint8_t)((y >> 8) & 0xFFu);
    out[5] = (uint8_t)(y & 0xFFu);
    out[6] = (uint8_t)((p >> 8) & 0xFFu);
    out[7] = (uint8_t)(p & 0xFFu);
    out[8] = (uint8_t)((r >> 8) & 0xFFu);
    out[9] = (uint8_t)(r & 0xFFu);

    out[10] = 0x00; /* reserved */

    /* [11..13]: 3-byte txid. */
    uint8_t tx0 = 0, tx1 = 0, tx2 = 0;
    if(txid) {
        tx0 = txid[0];
        tx1 = txid[1];
        tx2 = txid[2];
    }
    out[11] = tx0;
    out[12] = tx1;
    out[13] = tx2;

    out[14] = drone_bayang_checksum(out, BAYANG_PKT_LEN); /* [14]: checksum */
}

#endif /* !PUBLIC_BUILD */
