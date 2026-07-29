#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "defender_rogue_ap.h" /* DefenderBeaconRec */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pure 802.11 frame classifier — no ESP dependencies, only standard C headers.
 * Static inline so it can be tested independently on the host.
 */

typedef enum {
    DefWifiOther,       /* Unknown or unsupported frame type */
    DefWifiDeauth,      /* Deauthentication (management frame type 0, subtype 12) */
    DefWifiDisassoc,    /* Disassociation (management frame type 0, subtype 10) */
    DefWifiBeacon,      /* Beacon (management frame type 0, subtype 8) */
    DefWifiProbeResp,   /* Probe Response (management frame type 0, subtype 5) */
    DefWifiProbeReq,    /* Probe Request (management frame type 0, subtype 4) */
} DefWifiKind;

typedef struct {
    DefWifiKind kind;
    uint8_t addr1[6];           /* Destination address */
    uint8_t addr2[6];           /* Source address */
    uint8_t addr3[6];           /* BSSID (for management frames) */
    bool broadcast_addr1;       /* True if addr1 is all 0xFF */
    uint16_t reason_code;       /* Deauth/Disassoc reason (0 for others) */
} DefWifiFrame;

/**
 * Parse an 802.11 MAC header.
 *
 * Only management frames (type 0) get a specific kind; everything else → DefWifiOther
 * (but still parsed for addr1/2/3).
 *
 * @param p         Pointer to frame buffer (at least 24 bytes for minimal parse,
 *                  26 bytes to extract reason code)
 * @param len       Buffer length in bytes
 * @param out       Output structure (only filled if true returned)
 * @return          true if parse succeeded (len >= 24), false otherwise (no over-read,
 *                  out unchanged)
 *
 * Frame structure (bytes):
 *   [0..1]    Frame Control (little-endian: bits 0-3=type, 4-7=subtype)
 *   [2..3]    Duration/ID
 *   [4..9]    Address 1 (destination)
 *   [10..15]  Address 2 (source)
 *   [16..21]  Address 3 (BSSID for mgmt)
 *   [22..23]  Sequence/Fragment (for data frames)
 *   [24..25]  Reason code (deauth/disassoc only, little-endian)
 */
static inline bool defender_wifi_classify(const uint8_t* p, uint16_t len, DefWifiFrame* out) {
    if(len < 24) return false;

    /* Frame Control is little-endian: byte 0 = LSB, byte 1 = MSB */
    uint16_t fc = p[0] | (p[1] << 8);

    /* Extract type (bits 2-3) and subtype (bits 4-7) */
    uint8_t type = (fc & 0x0C) >> 2;
    uint8_t subtype = (fc & 0xF0) >> 4;

    /* Extract addresses */
    memcpy(out->addr1, &p[4], 6);
    memcpy(out->addr2, &p[10], 6);
    memcpy(out->addr3, &p[16], 6);

    /* Check if addr1 is broadcast (all 0xFF) */
    out->broadcast_addr1 = (out->addr1[0] == 0xFF && out->addr1[1] == 0xFF &&
                             out->addr1[2] == 0xFF && out->addr1[3] == 0xFF &&
                             out->addr1[4] == 0xFF && out->addr1[5] == 0xFF);

    /* Classify management frames (type == 0) */
    if(type == 0) {
        switch(subtype) {
            case 0x04:
                out->kind = DefWifiProbeReq;
                break;
            case 0x05:
                out->kind = DefWifiProbeResp;
                break;
            case 0x08:
                out->kind = DefWifiBeacon;
                break;
            case 0x0A:
                out->kind = DefWifiDisassoc;
                break;
            case 0x0C:
                out->kind = DefWifiDeauth;
                break;
            default:
                out->kind = DefWifiOther;
                break;
        }
    } else {
        out->kind = DefWifiOther;
    }

    /* Extract reason code for deauth/disassoc (bytes 24..25, little-endian) */
    if((out->kind == DefWifiDeauth || out->kind == DefWifiDisassoc) && len >= 26) {
        out->reason_code = p[24] | (p[25] << 8);
    } else {
        out->reason_code = 0;
    }

    return true;
}

/**
 * Firmware-level WiFi promiscuous tap interface.
 * Used by DefenderApp to monitor deauth/disassoc events.
 */

typedef struct {
    uint8_t bssid[6];           /* Target BSSID (addr3 from frame) */
    bool broadcast;             /* True if this was a broadcast deauth/disassoc */
    uint16_t reason;            /* Reason code from the frame */
    int8_t rssi;                /* Signal strength at time of capture */
    uint32_t ts_ms;             /* Timestamp in milliseconds (filled on drain) */
} DefTapDeauthRec;

/**
 * Start the WiFi promiscuous tap on the given channel.
 * Releases Bluetooth, initializes WiFi if needed, sets channel, and enables
 * management frame capture with the internal callback.
 *
 * @param channel  WiFi channel (1..14)
 */
void defender_wifi_tap_start(uint8_t channel);

/**
 * Stop the WiFi promiscuous tap.
 * Disables promiscuous mode and stops the WiFi stack.
 */
void defender_wifi_tap_stop(void);

/**
 * Change the WiFi channel while tap is active.
 *
 * @param ch  WiFi channel (1..14)
 */
void defender_wifi_tap_set_channel(uint8_t ch);

/**
 * Drain captured deauth/disassoc records from the ring buffer.
 * Should be called from the main app loop (not from ISR).
 * Timestamps records with current time on drain.
 *
 * @param out      Output buffer for records
 * @param max      Maximum number of records to return
 * @return         Number of records actually written to out
 */
uint32_t defender_wifi_tap_drain_deauth(DefTapDeauthRec* out, uint32_t max);

/**
 * Drain captured beacon records (SSID + BSSID + channel + RSSI) from the ring.
 * The same promiscuous session that feeds the deauth ring also fills this one.
 * Timestamps each record with the current tick on drain.
 *
 * @param out  Output buffer for beacon records
 * @param max  Maximum records to return
 * @return     Number of records written to out
 */
uint32_t defender_wifi_tap_drain_beacon(DefenderBeaconRec* out, uint32_t max);

#ifdef __cplusplus
}
#endif
