// applications/main/nrf24/helpers/nrf24_keysniffer.c
// KeySniffer-class decode: nRF24 payload -> USB HID keyboard report ->
// ASCII. Furi-free (see header comment). Only <stdint.h>/<stdbool.h> plus
// its own header are needed to host-compile/test this file.

#include "nrf24_keysniffer.h"

#include <stddef.h>

/* Standard USB HID Usage (keyboard/keypad page, 0x07) -> ASCII, unshifted.
 * Index = raw HID keycode (0x00..0x38 covers all keys this table needs to
 * resolve; anything outside that range, or any entry left at the default
 * 0, is "not printable"). Designated initializers -> unlisted entries are
 * implicitly 0. */
static const char ks_ascii_unshifted[0x39] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',

    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',

    [0x28] = '\n', /* Enter */
    [0x2B] = '\t', /* Tab */
    [0x2C] = ' ', /* Space */

    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/',
};

/* Same keycode range, SHIFT applied (letters uppercased, digit-row and
 * punctuation shifted to their standard US-layout symbol). */
static const char ks_ascii_shifted[0x39] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',

    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',

    [0x28] = '\n', /* Enter */
    [0x2B] = '\t', /* Tab */
    [0x2C] = ' ', /* Space */

    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}', [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>',
    [0x38] = '?',
};

/* USB HID Usage -> ASCII, honoring SHIFT (LeftShift 0x02 / RightShift
 * 0x20 in the modifier byte). Returns 0 if `hid` maps to no printable
 * char (including hid==0x00, "no key"). */
static char ks_hid_to_ascii(uint8_t hid, uint8_t modifier) {
    if(hid == 0 || hid >= (sizeof(ks_ascii_unshifted) / sizeof(ks_ascii_unshifted[0]))) {
        return 0;
    }
    bool shift = (modifier & (KS_MOD_LEFT_SHIFT | KS_MOD_RIGHT_SHIFT)) != 0;
    return shift ? ks_ascii_shifted[hid] : ks_ascii_unshifted[hid];
}

bool nrf24_keysniffer_decode(
    const Nrf24Capture* cap,
    char* out_ch,
    uint8_t* out_hid,
    uint8_t* out_mod) {
    if(!cap) {
        return false;
    }

    /* Report must fully fit within the payload at the documented offset. */
    if((size_t)KS_HID_REPORT_OFF + KS_HID_REPORT_LEN > sizeof(cap->payload)) {
        return false;
    }
    if(cap->len < KS_HID_REPORT_OFF + KS_HID_REPORT_LEN) {
        return false;
    }

    uint8_t modifier = cap->payload[KS_HID_REPORT_OFF + KS_HID_OFF_MODIFIER];
    uint8_t keycode = cap->payload[KS_HID_REPORT_OFF + KS_HID_OFF_KEY0];

    char ch = ks_hid_to_ascii(keycode, modifier);
    if(ch == 0) {
        return false;
    }

    if(out_ch) *out_ch = ch;
    if(out_hid) *out_hid = keycode;
    if(out_mod) *out_mod = modifier;
    return true;
}
