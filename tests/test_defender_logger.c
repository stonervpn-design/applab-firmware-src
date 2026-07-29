#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* Include the alert header to get DefenderAlert and category labels */
#include "../applications/main/defender/defender_alert.h"

/* Include the format function header */
#include "../applications/main/defender/defender_logger_fmt.h"

/**
 * Test 1: Plain alert with offender MAC and message.
 */
static void test_plain(void) {
    DefenderAlert a = {
        .cat = DefenderCatWifiDeauth,
        .severity = 2,
        .offender = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        .rssi = -42,
        .ts_ms = 1234567,
        .msg = "Deauth flood ch6 25/s"
    };

    char row[128];
    size_t n = defender_logger_format_row(row, sizeof(row), &a);

    assert(n > 0);
    const char* expected = "1234567,WIFI/DEAUTH,2,AA:BB:CC:DD:EE:FF,-42,Deauth flood ch6 25/s\n";
    assert(strcmp(row, expected) == 0);
    printf("test_plain: PASS\n");
}

/**
 * Test 2: Alert with all-zero offender (no MAC).
 */
static void test_no_offender(void) {
    DefenderAlert a = {
        .cat = DefenderCatSubghzJam,
        .severity = 2,
        .offender = {0, 0, 0, 0, 0, 0},
        .rssi = -30,
        .ts_ms = 9,
        .msg = "RF jam 433.92 +21dB"
    };

    char row[128];
    size_t n = defender_logger_format_row(row, sizeof(row), &a);

    assert(n > 0);
    const char* expected = "9,SUBGHZ/JAM,2,,-30,RF jam 433.92 +21dB\n";
    assert(strcmp(row, expected) == 0);
    printf("test_no_offender: PASS\n");
}

/**
 * Test 3: Message with comma (must be quoted).
 */
static void test_escape_comma(void) {
    DefenderAlert a = {
        .cat = DefenderCatWifiDeauth,
        .severity = 1,
        .offender = {0, 0, 0, 0, 0, 0},
        .rssi = -50,
        .ts_ms = 100,
        .msg = "a,b"
    };

    char row[128];
    size_t n = defender_logger_format_row(row, sizeof(row), &a);

    assert(n > 0);
    /* The message field should be "a,b" (quoted), and it should appear after the rssi comma */
    const char* expected = "100,WIFI/DEAUTH,1,,-50,\"a,b\"\n";
    assert(strcmp(row, expected) == 0);
    printf("test_escape_comma: PASS\n");
}

/**
 * Test 4: Message with quotes (must be doubled).
 */
static void test_escape_quote(void) {
    DefenderAlert a = {
        .cat = DefenderCatWifiDeauth,
        .severity = 1,
        .offender = {0, 0, 0, 0, 0, 0},
        .rssi = -50,
        .ts_ms = 200,
        .msg = "He said \"hi\""
    };

    char row[128];
    size_t n = defender_logger_format_row(row, sizeof(row), &a);

    assert(n > 0);
    /* The message should be "He said ""hi""" (quotes doubled) */
    const char* expected = "200,WIFI/DEAUTH,1,,-50,\"He said \"\"hi\"\"\"\n";
    assert(strcmp(row, expected) == 0);
    printf("test_escape_quote: PASS\n");
}

/**
 * Test 5: Overflow detection - buffer too small.
 */
static void test_overflow(void) {
    DefenderAlert a = {
        .cat = DefenderCatWifiDeauth,
        .severity = 2,
        .offender = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        .rssi = -42,
        .ts_ms = 1234567,
        .msg = "Deauth flood ch6 25/s"
    };

    /* Use a small buffer (8 bytes) */
    char row[16];
    memset(row, 0x7F, sizeof(row));  /* Sentinel fill */

    size_t n = defender_logger_format_row(row, 8, &a);

    /* Should return 0 for overflow */
    assert(n == 0);

    /* Check that we didn't write past index 8 */
    for(size_t i = 8; i < sizeof(row); i++) {
        assert(row[i] == 0x7F);
    }

    printf("test_overflow: PASS\n");
}

int main(void) {
    test_plain();
    test_no_offender();
    test_escape_comma();
    test_escape_quote();
    test_overflow();

    printf("ALL DEFENDER LOGGER TESTS PASSED\n");
    return 0;
}
