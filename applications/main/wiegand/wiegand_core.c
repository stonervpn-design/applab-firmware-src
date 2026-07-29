#include "wiegand_core.h"
#include <string.h>

/**
 * Count the number of 1s in a bit range.
 */
static uint8_t count_ones(const uint8_t* bits, int start, int end) {
    uint8_t count = 0;
    for (int i = start; i < end; i++) {
        count += (bits[i] & 1);
    }
    return count;
}

bool wiegand_verify_parity_26(const uint8_t* bits) {
    if (!bits) return false;

    // Count ones in bits 1-24 (the data bits between P1 and P2)
    uint32_t ones = count_ones(bits, 1, 25);

    // P1 (bit 0) should have odd parity: (ones + P1) % 2 == 1
    // This means P1 should be (ones + 1) % 2
    if ((bits[0] & 1) != ((ones + 1) % 2)) {
        return false;  // P1 parity mismatch
    }

    // P2 (bit 25) should have even parity: (ones + P2) % 2 == 0
    // This means P2 should be ones % 2
    if ((bits[25] & 1) != (ones % 2)) {
        return false;  // P2 parity mismatch
    }

    return true;
}

bool wiegand_verify_parity_34(const uint8_t* bits) {
    if (!bits) return false;

    // Count ones in bits 1-32 (the data bits between P1 and P2)
    uint32_t ones = count_ones(bits, 1, 33);

    // P1 (bit 0) should have odd parity: (ones + P1) % 2 == 1
    // This means P1 should be (ones + 1) % 2
    if ((bits[0] & 1) != ((ones + 1) % 2)) {
        return false;  // P1 parity mismatch
    }

    // P2 (bit 33) should have even parity: (ones + P2) % 2 == 0
    // This means P2 should be ones % 2
    if ((bits[33] & 1) != (ones % 2)) {
        return false;  // P2 parity mismatch
    }

    return true;
}

WiegandCredential wiegand_decode(const uint8_t* pulse_bits, uint8_t bit_count) {
    WiegandCredential cred = {0};

    if (!pulse_bits || (bit_count != 26 && bit_count != 34)) {
        cred.parity_ok = 0;
        cred.bits = 0;
        return cred;
    }

    cred.bits = bit_count;

    // Extract facility code from bits 1-8 (8 bits, MSB first)
    cred.facility = 0;
    for (int i = 0; i < 8; i++) {
        cred.facility = (cred.facility << 1) | (pulse_bits[1 + i] & 1);
    }

    // Extract card number from bits 9-24 (16 bits, MSB first)
    cred.card = 0;
    for (int i = 0; i < 16; i++) {
        cred.card = (cred.card << 1) | (pulse_bits[9 + i] & 1);
    }

    // Verify parity based on format
    if (bit_count == 26) {
        cred.parity_ok = wiegand_verify_parity_26(pulse_bits) ? 1 : 0;
    } else {
        cred.parity_ok = wiegand_verify_parity_34(pulse_bits) ? 1 : 0;
    }

    return cred;
}

uint8_t wiegand_encode_26(uint32_t facility, uint32_t card, uint8_t* out_bits) {
    if (!out_bits || facility > 255 || card > 65535) {
        return 0;  // Invalid parameters
    }

    memset(out_bits, 0, 26);

    // Bits 1-8: facility code (MSB first)
    for (int i = 0; i < 8; i++) {
        out_bits[1 + i] = (facility >> (7 - i)) & 1;
    }

    // Bits 9-24: card number (MSB first)
    for (int i = 0; i < 16; i++) {
        out_bits[9 + i] = (card >> (15 - i)) & 1;
    }

    // Bit 0 (P1): odd parity of bits 1-24
    uint8_t ones = count_ones(out_bits, 1, 25);
    out_bits[0] = (ones + 1) % 2;  // Make total (ones + P1) odd

    // Bit 25 (P2): even parity of bits 1-24
    out_bits[25] = ones % 2;  // Make total (ones + P2) even

    return 26;
}

uint8_t wiegand_encode_34(uint32_t facility, uint32_t card, uint8_t* out_bits) {
    if (!out_bits || facility > 255 || card > 65535) {
        return 0;  // Invalid parameters
    }

    memset(out_bits, 0, 34);

    // Bits 1-8: facility code (MSB first)
    for (int i = 0; i < 8; i++) {
        out_bits[1 + i] = (facility >> (7 - i)) & 1;
    }

    // Bits 9-24: card number (MSB first)
    for (int i = 0; i < 16; i++) {
        out_bits[9 + i] = (card >> (15 - i)) & 1;
    }

    // Bits 25-26: reserved (already 0 from memset)
    out_bits[25] = 0;
    out_bits[26] = 0;

    // Bit 0 (P1): odd parity of bits 1-32
    uint8_t ones = count_ones(out_bits, 1, 33);
    out_bits[0] = (ones + 1) % 2;  // Make total (ones + P1) odd

    // Bit 33 (P2): even parity of bits 1-32
    out_bits[33] = ones % 2;  // Make total (ones + P2) even

    return 34;
}
