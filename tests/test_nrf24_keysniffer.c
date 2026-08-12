// tests/test_nrf24_keysniffer.c
// Host unit test for the KeySniffer decode helper (Task 3 of the nRF24
// Red-Team Suite). Compile+run under WSL:
//   wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" &&
//     gcc -std=gnu11 -Wall -Wextra -Iapplications/main/nrf24/helpers
//     tests/test_nrf24_keysniffer.c applications/main/nrf24/helpers/nrf24_keysniffer.c
//     -o /tmp/tks && /tmp/tks'

#include "nrf24_keysniffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Build a capture with modifier/keycode placed at KS_HID_REPORT_OFF, full
 * default length (KS_HID_REPORT_OFF + KS_HID_REPORT_LEN, i.e. the whole
 * 8-byte report present). */
static Nrf24Capture make_cap(uint8_t modifier, uint8_t keycode) {
    Nrf24Capture c;
    memset(&c, 0, sizeof c);
    c.channel = 5;
    c.payload[KS_HID_REPORT_OFF + KS_HID_OFF_MODIFIER] = modifier;
    c.payload[KS_HID_REPORT_OFF + KS_HID_OFF_KEY0] = keycode;
    c.len = KS_HID_REPORT_OFF + KS_HID_REPORT_LEN;
    c.ts = 42;
    c.proto = N24_KEYBOARD;
    return c;
}

static void expect_char(uint8_t modifier, uint8_t keycode, char expected) {
    Nrf24Capture c = make_cap(modifier, keycode);
    char ch = 0;
    uint8_t hid = 0, mod = 0;
    bool ok = nrf24_keysniffer_decode(&c, &ch, &hid, &mod);
    assert(ok);
    assert(ch == expected);
    assert(hid == keycode);
    assert(mod == modifier);
}

static void test_letter_a(void) {
    expect_char(0x00, 0x04, 'a');
    printf("test_letter_a OK\n");
}

static void test_letter_a_shifted(void) {
    expect_char(0x02, 0x04, 'A'); /* LeftShift */
    printf("test_letter_a_shifted OK\n");
}

static void test_digit_1(void) {
    expect_char(0x00, 0x1E, '1');
    printf("test_digit_1 OK\n");
}

static void test_digit_1_shifted(void) {
    expect_char(0x20, 0x1E, '!'); /* RightShift */
    printf("test_digit_1_shifted OK\n");
}

static void test_space(void) {
    expect_char(0x00, 0x2C, ' ');
    printf("test_space OK\n");
}

static void test_enter(void) {
    expect_char(0x00, 0x28, '\n');
    printf("test_enter OK\n");
}

static void test_no_key_returns_false(void) {
    Nrf24Capture c = make_cap(0x00, 0x00);
    char ch = 'X';
    uint8_t hid = 0xFF, mod = 0xFF;
    bool ok = nrf24_keysniffer_decode(&c, &ch, &hid, &mod);
    assert(!ok);
    printf("test_no_key_returns_false OK\n");
}

static void test_too_short_returns_false(void) {
    Nrf24Capture c = make_cap(0x00, 0x04); /* would decode to 'a' if long enough */
    c.len = KS_HID_REPORT_OFF + 1; /* far short of the full 8-byte report */
    char ch = 'X';
    bool ok = nrf24_keysniffer_decode(&c, &ch, NULL, NULL);
    assert(!ok);
    assert(ch == 'X'); /* untouched on failure */
    printf("test_too_short_returns_false OK\n");
}

static void test_null_cap_returns_false(void) {
    char ch = 'X';
    bool ok = nrf24_keysniffer_decode(NULL, &ch, NULL, NULL);
    assert(!ok);
    printf("test_null_cap_returns_false OK\n");
}

static void test_null_out_params_dont_crash(void) {
    Nrf24Capture c = make_cap(0x00, 0x04);
    bool ok = nrf24_keysniffer_decode(&c, NULL, NULL, NULL);
    assert(ok);
    printf("test_null_out_params_dont_crash OK\n");
}

static void test_garbage_keycode_returns_false(void) {
    /* keycode 0xFF is well outside the mapped table range. */
    Nrf24Capture c = make_cap(0x00, 0xFF);
    char ch = 'X';
    bool ok = nrf24_keysniffer_decode(&c, &ch, NULL, NULL);
    assert(!ok);
    printf("test_garbage_keycode_returns_false OK\n");
}

int main(void) {
    test_letter_a();
    test_letter_a_shifted();
    test_digit_1();
    test_digit_1_shifted();
    test_space();
    test_enter();
    test_no_key_returns_false();
    test_too_short_returns_false();
    test_null_cap_returns_false();
    test_null_out_params_dont_crash();
    test_garbage_keycode_returns_false();
    printf("test_nrf24_keysniffer: ALL PASS\n");
    return 0;
}
