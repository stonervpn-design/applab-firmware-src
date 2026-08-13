#pragma once

#include <stdint.h>

/* 802.11 management-frame base builders (WF1 Task 2).
 *
 * Furi-free by design (host-testable in isolation): only libc headers.
 * Hand-packed byte layout, no radio/HAL dependency -- the scene layer
 * feeds the returned frame to wlan_hal_send_raw() (raw) or to
 * wlan_mgmt_fuzz_next() (mutated) itself.
 *
 * Auth/Assoc-Request are NOT duplicated here -- they already exist as
 * pmkid_build_auth_open()/pmkid_build_assoc_req() in pmkid_frames.h and
 * are reused as-is by the scene layer (Task 3).
 *
 * Common byte layout for every builder below: frame-control at [0-1],
 * duration [2-3], addr1 [4-9], addr2 [10-15], addr3 [16-21], seqctl
 * [22-23], then the frame-type-specific fixed fields / IEs from [24..]. */

/* Build an 802.11 Probe Request from `src`, addressed to the broadcast
 * BSSID, carrying a Probe-directed SSID tag + supported-rates tag.
 * Returns total length written to `out`, or 0 if `cap` is too small. */
uint16_t wlan_build_probe_req(uint8_t* out, uint16_t cap, const uint8_t src[6], const char* ssid);

/* Build an 802.11 Beacon from `bssid`, addressed to the broadcast RA,
 * carrying timestamp/interval/capability fixed fields, an SSID tag, a
 * supported-rates tag, and a DS-Parameter-Set tag set to `channel`.
 * Returns total length written to `out`, or 0 if `cap` is too small. */
uint16_t
    wlan_build_beacon(uint8_t* out, uint16_t cap, const uint8_t bssid[6], const char* ssid, uint8_t channel);

/* Build an 802.11 Deauthentication frame from `bssid` targeting `sta`
 * (addr1 = sta, addr2 = addr3 = bssid), with `reason` written
 * little-endian as the 2-byte frame body. Layout mirrors
 * scene_network_deauth.c's deauth_tmpl[26]. Returns total length written
 * to `out` (always 26 on success), or 0 if `cap` is too small. */
uint16_t wlan_build_deauth(
    uint8_t* out,
    uint16_t cap,
    const uint8_t bssid[6],
    const uint8_t sta[6],
    uint16_t reason);
