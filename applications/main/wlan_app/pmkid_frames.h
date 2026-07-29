#pragma once

#include <stdint.h>
#include <stddef.h>

/* Build an 802.11 open-system Authentication frame (seq 1) addressed to `bssid`
 * from `src`. Returns total length written to `out`, or 0 if `cap` is too small.
 * Layout: 24-byte MAC header + 6-byte auth body = 30 bytes. */
size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]);

/* Build an 802.11 Association Request to `bssid` from `src`, advertising an RSN
 * IE (CCMP group + pairwise, PSK AKM) so the AP returns an RSN PMKID in EAPOL
 * M1. `ssid` is the target's SSID (<=32 chars). Returns total length written, or
 * 0 if `cap` is too small. Max length with a 32-char SSID is 94 bytes. */
size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid);
