#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "../applications/main/wiegand/wiegand_core.h"

/**
 * Test 1: Decode valid 26-bit Wiegand with facility=1, card=65535
 * Format: P1 + 8*facility + 16*card + P2
 */
void test_decode_26_bit_valid() {
    printf("TEST: decode_26_bit_valid...\n");

    // Construct a 26-bit frame: facility=1, card=65535
    // Facility=1: 00000001 (1 one)
    // Card=65535: 1111111111111111 (16 ones)
    // Data bits (1-24): 00000001 1111111111111111
    // Count ones: 1 + 16 = 17
    // P1 (odd parity): P1 = (17 + 1) % 2 = 0
    // P2 (even parity): P2 = 17 % 2 = 1
    uint8_t bits_26[26] = {
        0,  // P1 (odd parity)
        0,0,0,0,0,0,0,1,  // facility = 1
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  // card = 65535
        1   // P2 (even parity)
    };

    WiegandCredential cred = wiegand_decode(bits_26, 26);
    printf("  facility=%u, card=%u, parity_ok=%u, bits=%u\n", cred.facility, cred.card, cred.parity_ok, cred.bits);

    assert(cred.facility == 1);
    assert(cred.card == 65535);
    assert(cred.parity_ok == 1);
    assert(cred.bits == 26);
    printf("  PASSED\n");
}

/**
 * Test 2: Decode valid 34-bit Wiegand
 */
void test_decode_34_bit_valid() {
    printf("TEST: decode_34_bit_valid...\n");

    // Generate valid 34-bit frame using encode, then decode it
    uint8_t bits_34[34];
    uint8_t result = wiegand_encode_34(99, 12345, bits_34);
    assert(result == 34);

    WiegandCredential cred = wiegand_decode(bits_34, 34);
    printf("  facility=%u, card=%u, parity_ok=%u, bits=%u\n", cred.facility, cred.card, cred.parity_ok, cred.bits);

    assert(cred.facility == 99);
    assert(cred.card == 12345);
    assert(cred.parity_ok == 1);
    assert(cred.bits == 34);
    printf("  PASSED\n");
}

/**
 * Test 3: Detect bad parity in 26-bit
 */
void test_decode_26_bad_parity() {
    printf("TEST: decode_26_bad_parity...\n");

    // Same as test 1 but with wrong P2
    uint8_t bits_26[26] = {
        0,  // P1
        0,0,0,0,0,0,0,1,  // facility = 1
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  // card = 65535
        0   // P2 (WRONG - should be 1)
    };

    WiegandCredential cred = wiegand_decode(bits_26, 26);
    printf("  parity_ok=%u (should be 0)\n", cred.parity_ok);
    assert(cred.parity_ok == 0);
    printf("  PASSED\n");
}

/**
 * Test 4: Encode/decode round-trip for 26-bit
 */
void test_encode_decode_26_roundtrip() {
    printf("TEST: encode_decode_26_roundtrip...\n");

    uint32_t facility_in = 42;
    uint32_t card_in = 12345;

    // Encode
    uint8_t encoded[26];
    uint8_t result = wiegand_encode_26(facility_in, card_in, encoded);
    assert(result == 26);
    printf("  encoded: %u bits\n", result);

    // Decode
    WiegandCredential cred = wiegand_decode(encoded, 26);
    printf("  decoded: facility=%u, card=%u, parity_ok=%u\n", cred.facility, cred.card, cred.parity_ok);

    assert(cred.facility == facility_in);
    assert(cred.card == card_in);
    assert(cred.parity_ok == 1);
    printf("  PASSED\n");
}

/**
 * Test 5: Encode/decode round-trip for 34-bit
 */
void test_encode_decode_34_roundtrip() {
    printf("TEST: encode_decode_34_roundtrip...\n");

    uint32_t facility_in = 255;
    uint32_t card_in = 65535;

    // Encode
    uint8_t encoded[34];
    uint8_t result = wiegand_encode_34(facility_in, card_in, encoded);
    assert(result == 34);
    printf("  encoded: %u bits\n", result);

    // Decode
    WiegandCredential cred = wiegand_decode(encoded, 34);
    printf("  decoded: facility=%u, card=%u, parity_ok=%u\n", cred.facility, cred.card, cred.parity_ok);

    assert(cred.facility == facility_in);
    assert(cred.card == card_in);
    assert(cred.parity_ok == 1);
    printf("  PASSED\n");
}

/**
 * Test 6: Invalid bit count
 */
void test_decode_invalid_bitcount() {
    printf("TEST: decode_invalid_bitcount...\n");

    uint8_t bits[32] = {0};
    WiegandCredential cred = wiegand_decode(bits, 32);  // Invalid: must be 26 or 34
    printf("  parity_ok=%u, bits=%u (should be 0)\n", cred.parity_ok, cred.bits);

    assert(cred.parity_ok == 0);
    assert(cred.bits == 0);
    printf("  PASSED\n");
}

/**
 * Test 7: NULL pointer handling
 */
void test_decode_null_pointer() {
    printf("TEST: decode_null_pointer...\n");

    WiegandCredential cred = wiegand_decode(NULL, 26);
    printf("  parity_ok=%u (should be 0)\n", cred.parity_ok);

    assert(cred.parity_ok == 0);
    printf("  PASSED\n");
}

/**
 * Test 8: Encode with invalid facility (> 255)
 */
void test_encode_26_invalid_facility() {
    printf("TEST: encode_26_invalid_facility...\n");

    uint8_t bits[26];
    uint8_t result = wiegand_encode_26(256, 100, bits);  // facility > 255
    printf("  result=%u (should be 0)\n", result);

    assert(result == 0);
    printf("  PASSED\n");
}

/**
 * Test 9: Encode with invalid card (> 65535)
 */
void test_encode_26_invalid_card() {
    printf("TEST: encode_26_invalid_card...\n");

    uint8_t bits[26];
    uint8_t result = wiegand_encode_26(100, 65536, bits);  // card > 65535
    printf("  result=%u (should be 0)\n", result);

    assert(result == 0);
    printf("  PASSED\n");
}

/**
 * Test 10: Encode with NULL buffer
 */
void test_encode_26_null_buffer() {
    printf("TEST: encode_26_null_buffer...\n");

    uint8_t result = wiegand_encode_26(100, 200, NULL);
    printf("  result=%u (should be 0)\n", result);

    assert(result == 0);
    printf("  PASSED\n");
}

/**
 * Test 11: Edge case - facility=0, card=0
 */
void test_encode_decode_zeros() {
    printf("TEST: encode_decode_zeros...\n");

    uint8_t encoded[26];
    uint8_t result = wiegand_encode_26(0, 0, encoded);
    assert(result == 26);

    WiegandCredential cred = wiegand_decode(encoded, 26);
    printf("  facility=%u, card=%u, parity_ok=%u\n", cred.facility, cred.card, cred.parity_ok);

    assert(cred.facility == 0);
    assert(cred.card == 0);
    assert(cred.parity_ok == 1);
    printf("  PASSED\n");
}

/**
 * Test 12: Verify parity functions directly for 26-bit
 */
void test_verify_parity_26_direct() {
    printf("TEST: verify_parity_26_direct...\n");

    // Data: facility=1, card=65535
    // ones in data = 17
    // P1 = (17+1)%2 = 0
    // P2 = 17%2 = 1
    uint8_t valid_bits[26] = {
        0, 0,0,0,0,0,0,0,1, 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0, 1
    };
    bool valid = wiegand_verify_parity_26(valid_bits);
    printf("  parity_ok=%d (should be 1)\n", valid);
    assert(valid == true);

    // Flip P1 to make it invalid
    valid_bits[0] = 1;
    valid = wiegand_verify_parity_26(valid_bits);
    printf("  parity_ok=%d after flipping P1 (should be 0)\n", valid);
    assert(valid == false);

    printf("  PASSED\n");
}

/**
 * Test 13: Verify parity functions directly for 34-bit
 */
void test_verify_parity_34_direct() {
    printf("TEST: verify_parity_34_direct...\n");

    uint8_t valid_bits[34];
    memset(valid_bits, 0, sizeof(valid_bits));
    // Encode then verify
    wiegand_encode_34(200, 30000, valid_bits);

    bool valid = wiegand_verify_parity_34(valid_bits);
    printf("  parity_ok=%d (should be 1)\n", valid);
    assert(valid == true);

    // Flip a data bit (bit 5)
    valid_bits[5] ^= 1;
    valid = wiegand_verify_parity_34(valid_bits);
    printf("  parity_ok=%d after flipping data bit (should be 0)\n", valid);
    assert(valid == false);

    printf("  PASSED\n");
}

int main() {
    printf("=== WIEGAND CORE TESTS ===\n\n");

    test_decode_26_bit_valid();
    test_decode_34_bit_valid();
    test_decode_26_bad_parity();
    test_encode_decode_26_roundtrip();
    test_encode_decode_34_roundtrip();
    test_decode_invalid_bitcount();
    test_decode_null_pointer();
    test_encode_26_invalid_facility();
    test_encode_26_invalid_card();
    test_encode_26_null_buffer();
    test_encode_decode_zeros();
    test_verify_parity_26_direct();
    test_verify_parity_34_direct();

    printf("\n=== ALL WIEGAND CORE TESTS PASSED ===\n");
    return 0;
}
