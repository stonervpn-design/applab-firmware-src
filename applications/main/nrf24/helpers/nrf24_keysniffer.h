#pragma once

#include "nrf24_capture.h"

#include <stdbool.h>
#include <stdint.h>

/* KeySniffer-class decode: unencrypted wireless-keyboard nRF24 payload ->
 * USB HID keyboard report -> ASCII.
 *
 * Furi-free by design (host-testable in isolation): only pulls in
 * nrf24_capture.h (itself furi-free) + libc headers.
 *
 * Scope (v1, honest): the vulnerable-keyboard classes this targets
 * (KeySniffer/MoKeyboard-style unencrypted dongles) carry a standard
 * 8-byte USB HID keyboard report [modifier, reserved, key0..key5] inside
 * the nRF24 payload at a fixed byte offset. This module implements the
 * generic "8-byte HID report at a documented offset" model plus the full
 * standard USB HID Usage -> ASCII table (with SHIFT). It does NOT claim
 * any specific keyboard model's proprietary framing beyond that generic
 * layout -- the offset below is a documented default and is
 * HW-refinable once real captures are available. */

/* Byte offset of the 8-byte HID report [mod, reserved, key0..key5] within
 * Nrf24Capture.payload. Default matches the common KeySniffer-class
 * layout (address/frame-type prelude before the raw HID report).
 * HW-REFINABLE: adjust once real vulnerable-keyboard captures are on
 * hand to confirm/adjust the prelude length for the specific dongle(s)
 * targeted. */
#define KS_HID_REPORT_OFF 2

/* HID report layout (all relative to KS_HID_REPORT_OFF). */
#define KS_HID_REPORT_LEN 8
#define KS_HID_OFF_MODIFIER 0
#define KS_HID_OFF_RESERVED 1
#define KS_HID_OFF_KEY0 2

/* HID modifier bits (relevant subset). */
#define KS_MOD_LEFT_SHIFT 0x02
#define KS_MOD_RIGHT_SHIFT 0x20

/* Decode a captured nRF24 payload as a KeySniffer-class unencrypted HID
 * keyboard report and translate the first keycode to ASCII.
 *
 * Reads modifier = payload[KS_HID_REPORT_OFF + KS_HID_OFF_MODIFIER] and
 * keycode = payload[KS_HID_REPORT_OFF + KS_HID_OFF_KEY0], applies SHIFT
 * per the standard USB HID Usage table, and returns the resulting ASCII
 * char.
 *
 * Returns false (leaving any non-NULL out-params untouched) if:
 *   - cap is NULL,
 *   - cap->len is too short to contain the report at KS_HID_REPORT_OFF,
 *   - the keycode is 0x00 (no key) or otherwise maps to no printable char.
 *
 * On success, writes *out_ch (ASCII), *out_hid (raw keycode), *out_mod
 * (modifier byte) for any of the three that are non-NULL, and returns
 * true. */
bool nrf24_keysniffer_decode(const Nrf24Capture* cap, char* out_ch, uint8_t* out_hid, uint8_t* out_mod);
