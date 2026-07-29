#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Wiegand credential structure for 26-bit and 34-bit formats.
 * 26-bit: [P1:1] [Facility:8] [Card:16] [P2:1]
 * 34-bit: [P1:1] [Facility:8] [Card:16] [Reserved:2] [P2:1]
 */
typedef struct {
    uint32_t facility;      // 8-bit facility code
    uint32_t card;          // 16-bit card number
    uint8_t parity_ok;      // 1=both parities valid, 0=invalid
    uint8_t bits;           // 26 or 34
} WiegandCredential;

/**
 * Decode a Wiegand pulse train into facility/card credentials.
 * @param pulse_bits Array of uint8_t (1 or 0 per pulse, bit 0 = first pulse/P1)
 * @param bit_count 26 or 34 bits
 * @return WiegandCredential with facility/card/parity_ok/bits filled
 */
WiegandCredential wiegand_decode(const uint8_t* pulse_bits, uint8_t bit_count);

/**
 * Verify 26-bit Wiegand parity.
 * P1 (bit 0) = odd parity over bits 1-24
 * P2 (bit 25) = even parity over bits 1-24
 * @param bits 26-element uint8_t array
 * @return true if both parities are valid
 */
bool wiegand_verify_parity_26(const uint8_t* bits);

/**
 * Verify 34-bit Wiegand parity.
 * P1 (bit 0) = odd parity over bits 1-32
 * P2 (bit 33) = even parity over bits 1-32
 * @param bits 34-element uint8_t array
 * @return true if both parities are valid
 */
bool wiegand_verify_parity_34(const uint8_t* bits);

/**
 * Encode facility + card into 26-bit Wiegand pulse sequence.
 * @param facility 0-255 facility code
 * @param card 0-65535 card number
 * @param out_bits Output buffer (must hold 26 bytes, one bit per byte)
 * @return 26 on success, 0 on error (invalid params or null buffer)
 */
uint8_t wiegand_encode_26(uint32_t facility, uint32_t card, uint8_t* out_bits);

/**
 * Encode facility + card into 34-bit Wiegand pulse sequence.
 * @param facility 0-255 facility code
 * @param card 0-65535 card number
 * @param out_bits Output buffer (must hold 34 bytes, one bit per byte)
 * @return 34 on success, 0 on error (invalid params or null buffer)
 */
uint8_t wiegand_encode_34(uint32_t facility, uint32_t card, uint8_t* out_bits);
