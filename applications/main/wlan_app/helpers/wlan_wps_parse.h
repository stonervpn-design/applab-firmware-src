#pragma once

#include <stdbool.h>
#include <stdint.h>

/* WPS (Wi-Fi Protected Setup) Information-Element parser (WF2 Task 1).
 *
 * Furi-free by design (host-testable in isolation): only libc headers.
 * Pure, passive byte-parsing -- given the tagged-IE region of a captured
 * beacon/probe-response body, reports whether the AP advertises WPS and,
 * if so, its posture (locked / config methods / version / state). Does
 * NOT transmit, associate, or attempt any PIN/Pixie-Dust attack -- that is
 * explicitly off-device; this is recon only.
 *
 * Bounds-hardened: every IE (id, len) and every WPS attribute (2-byte BE
 * type, 2-byte BE length, value) is validated against the remaining
 * buffer before being read. A truncated or over-claiming-length input
 * terminates the walk safely and never reads past `ies_len`. */

/* WPS vendor-IE identity: element id 0xDD (vendor specific), OUI
 * 00:50:F2, vendor type 0x04 (WPS). */
#define WLAN_WPS_IE_ID 0xDD
#define WLAN_WPS_OUI_0 0x00
#define WLAN_WPS_OUI_1 0x50
#define WLAN_WPS_OUI_2 0xF2
#define WLAN_WPS_OUI_TYPE 0x04

/* WPS attribute IDs (subset relevant to recon posture). */
#define WLAN_WPS_ATTR_VERSION 0x104A
#define WLAN_WPS_ATTR_WPS_STATE 0x1044
#define WLAN_WPS_ATTR_AP_SETUP_LOCKED 0x1057
#define WLAN_WPS_ATTR_SELECTED_REGISTRAR 0x1041
#define WLAN_WPS_ATTR_CONFIG_METHODS 0x1008

/* Config-Methods (0x1008) bitmask values. */
#define WLAN_WPS_CM_USBA 0x0001
#define WLAN_WPS_CM_ETHERNET 0x0002
#define WLAN_WPS_CM_LABEL 0x0004
#define WLAN_WPS_CM_DISPLAY 0x0008
#define WLAN_WPS_CM_EXT_NFC_TOKEN 0x0010
#define WLAN_WPS_CM_INT_NFC_TOKEN 0x0020
#define WLAN_WPS_CM_NFC_INTERFACE 0x0040
#define WLAN_WPS_CM_PUSHBUTTON 0x0080
#define WLAN_WPS_CM_KEYPAD 0x0100

/* wps_state (0x1044) values. */
#define WLAN_WPS_STATE_NOT_CONFIGURED 0x01
#define WLAN_WPS_STATE_CONFIGURED 0x02

typedef struct {
    bool present; /* a WPS IE (OUI 00:50:F2 type 0x04) was found */
    bool locked; /* AP-Setup-Locked (0x1057) == 1 */
    bool selected_registrar; /* Selected-Registrar (0x1041) == 1 */
    uint16_t config_methods; /* 0x1008 bitmask (PBC 0x0080, Label 0x0004, Display 0x0008, Keypad 0x0100, ...) */
    uint8_t version; /* 0x104A (0x10 = 1.0; 0x20 attr via WFA ext for 2.0) */
    uint8_t wps_state; /* 0x1044 (1=not configured, 2=configured) */
} WlanWpsInfo;

/* Parse the tagged-IE region of a beacon/probe-response body. Zero-inits
 * *out, then fills it if a WPS IE is present. Returns out->present.
 *
 * Never reads past ies[0 .. ies_len). A malformed/truncated IE or WPS
 * attribute (one whose claimed length would run past the remaining
 * buffer) terminates the walk at that point rather than reading OOB. */
bool wlan_wps_parse_ies(const uint8_t* ies, uint16_t ies_len, WlanWpsInfo* out);
