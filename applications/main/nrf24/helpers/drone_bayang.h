/* Bayang toy-quadcopter nRF24 packet builder -- pure, furi-free leaf.
 *
 * SOURCE / STATUS: the wire format below is TRANSCRIBED from the public GPL
 * Deviation TX firmware (`protocol/bayang.c`) and the Multiprotocol TX
 * module (`Bayang_nrf24l01.ino`) -- both widely-mirrored open-source
 * projects documenting the "Bayang" protocol used by a family of cheap
 * nRF24-based toy/micro quadcopters (e.g. Eachine, JJRC, Floss, H8-mini
 * clones). This is the SAME class of device as the public Deviation/
 * Multiprotocol transmitter apps openly support -- hobbyist toy-quad RC,
 * not any restricted category.
 *
 * THIS IS UNVERIFIED. It has NOT been tested against a real drone, a real
 * Bayang RX, or a captured-and-decoded reference frame. It was transcribed
 * from memory of the public source layout, not copy-pasted from a
 * byte-exact reference while writing this file. Every field this file is
 * not fully certain of is marked `// UNVERIFIED:` in drone_bayang.c --
 * treat every constant here as "best-effort public-doc transcription,
 * pending hardware/capture confirmation" until an on-air bind test against
 * a real receiver proves it. Host tests below only prove INTERNAL
 * consistency (checksum determinism, field packing, bounds, clamping) --
 * NOT real-frame correctness.
 *
 * Packet shape (15 bytes total, both bind and control):
 *   [0]      command byte: 0xA5 = control/data, 0xA4 = bind
 *   [1]      flags (control) / reserved 0x00 (bind) -- see BAYANG_FLAG_*
 *   [2..9]   control: 4x big-endian 16-bit axis fields (throttle, yaw,
 *            pitch, roll in that order), each clamped to [0, BAYANG_AXIS_MAX]
 *            -- bind: bytes [1..5] = 5-byte bind address, [6..9] = the 4
 *            RF hop channels
 *   [10]     reserved 0x00
 *   [11..13] 3-byte txid (present in both bind and control packets)
 *   [14]     checksum -- drone_bayang_checksum()
 *
 * Furi-free: only libc headers, no furi/HAL includes, no allocation.
 * Host-testable in isolation with -Iapplications/main/nrf24/helpers. */
#pragma once

#include <stdint.h>

#define BAYANG_PKT_LEN 15
#define BAYANG_RF_NUM_CH 4

/* Command byte (packet[0]). */
#define BAYANG_PKT_BIND 0xA4u
#define BAYANG_PKT_CONTROL 0xA5u

/* Documented per-axis range: throttle 0..0x3FF from 0; yaw/pitch/roll
 * 0..0x3FF around a 0x200 center (per the Reference in the ND1 plan,
 * transcribed from the public source's stick-axis convention). */
#define BAYANG_AXIS_MAX 0x3FFu
#define BAYANG_AXIS_CENTER 0x200u

/* Bayang flag bits (packet[1] on control packets), from the public
 * Deviation/Multiprotocol flags enum. FLIP is the bit this project is most
 * confident of (0x01, universally the first/lowest bit in every mirror of
 * this enum seen). HEADLESS and RTH share the same documented enum but
 * their exact bit position is NOT independently re-confirmed against the
 * source in this pass -- see the UNVERIFIED comment on each. */
#define BAYANG_FLAG_FLIP 0x01u
#define BAYANG_FLAG_HEADLESS 0x40u /* UNVERIFIED: bit position per recalled public enum, not re-confirmed */
#define BAYANG_FLAG_RTH 0x80u /* UNVERIFIED: bit position per recalled public enum, not re-confirmed */

/* Documented Bayang checksum: sum the first (len-1) bytes, add a fixed
 * 0x55 seed, truncate to 8 bits. `pkt`/`len` generalize the plan's
 * "sum i=0..13, seed +0x55" rule to any len (checksum always trails at
 * pkt[len-1] and sums pkt[0..len-2]). NULL pkt or len==0 returns the bare
 * seed byte (0x55) as a safe default -- never dereferences pkt in that
 * case. UNVERIFIED: the +0x55 seed is transcribed from the public source
 * as documented in the ND1 plan Reference; not independently re-verified
 * against bayang.c at implementation time. Host test locks whatever is
 * transcribed here for determinism. */
uint8_t drone_bayang_checksum(const uint8_t* pkt, uint8_t len);

/* Build a 15-byte Bayang bind packet for `txid` into `out`. NULL out is a
 * safe no-op. NULL txid packs 0x00 for all 3 txid bytes (still a valid,
 * bounded packet). `out` is always exactly BAYANG_PKT_LEN bytes; never
 * written past. */
void drone_bayang_build_bind(const uint8_t txid[3], uint8_t out[BAYANG_PKT_LEN]);

/* Build a 15-byte Bayang control packet for `txid` + the 4 axes + `flags`
 * into `out`. Each axis is clamped to the documented [0, BAYANG_AXIS_MAX]
 * protocol range before packing (so absurd inputs like 0xFFFF stay in the
 * field's documented 10-bit range). Buffer safety is independent of the
 * clamp: fixed byte indices + per-byte masks mean no input writes outside
 * `out`. NULL out is a safe no-op; NULL txid packs 0x00 for all 3 txid
 * bytes. `out` is always exactly BAYANG_PKT_LEN bytes; never written past. */
void drone_bayang_build_control(
    const uint8_t txid[3],
    uint16_t throttle,
    uint16_t yaw,
    uint16_t pitch,
    uint16_t roll,
    uint8_t flags,
    uint8_t out[BAYANG_PKT_LEN]);

/* Derive the 4 RF hop channels for `txid` into `out_ch`. Every entry is
 * always <= 125 (nRF24 2.4GHz channel range), regardless of txid. NULL
 * out_ch is a safe no-op; NULL txid derives channels from an all-zero
 * txid. UNVERIFIED: the exact derivation formula (which bytes/weights
 * produce which channel) is NOT confirmed against the real source -- only
 * the documented FACT "4 channels derived from txid, each <= 125" is
 * structurally guaranteed here. */
void drone_bayang_rf_channels(const uint8_t txid[3], uint8_t out_ch[BAYANG_RF_NUM_CH]);
