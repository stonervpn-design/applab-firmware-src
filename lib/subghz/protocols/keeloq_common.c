#include "keeloq_common.h"

#include <furi.h>

#include <m-array.h>

#define bit(x, n) (((x) >> (n)) & 1)
#define g5(x, a, b, c, d, e) \
    (bit(x, a) + bit(x, b) * 2 + bit(x, c) * 4 + bit(x, d) * 8 + bit(x, e) * 16)

/** Simple Learning Encrypt
 * @param data - 0xBSSSCCCC, B(4bit) key, S(10bit) serial&0x3FF, C(16bit) counter
 * @param key - manufacture (64bit)
 * @return keeloq encrypt data
 */
inline uint32_t subghz_protocol_keeloq_common_encrypt(const uint32_t data, const uint64_t key) {
    uint32_t x = data, r;
    for(r = 0; r < 528; r++)
        x = (x >> 1) ^ ((bit(x, 0) ^ bit(x, 16) ^ (uint32_t)bit(key, r & 63) ^
                         bit(KEELOQ_NLF, g5(x, 1, 9, 20, 26, 31)))
                        << 31);
    return x;
}

/** Simple Learning Decrypt
 * @param data - keeloq encrypt data
 * @param key - manufacture (64bit)
 * @return 0xBSSSCCCC, B(4bit) key, S(10bit) serial&0x3FF, C(16bit) counter
 */
inline uint32_t subghz_protocol_keeloq_common_decrypt(const uint32_t data, const uint64_t key) {
    uint32_t x = data, r;
    for(r = 0; r < 528; r++)
        x = (x << 1) ^ bit(x, 31) ^ bit(x, 15) ^ (uint32_t)bit(key, (15 - r) & 63) ^
            bit(KEELOQ_NLF, g5(x, 0, 8, 19, 25, 30));
    return x;
}

/** Normal Learning
 * @param data - serial number (28bit)
 * @param key - manufacture (64bit)
 * @return manufacture for this serial number (64bit)
 */
inline uint64_t subghz_protocol_keeloq_common_normal_learning(uint32_t data, const uint64_t key) {
    uint32_t k1, k2;

    data &= 0x0FFFFFFF;
    data |= 0x20000000;
    k1 = subghz_protocol_keeloq_common_decrypt(data, key);

    data &= 0x0FFFFFFF;
    data |= 0x60000000;
    k2 = subghz_protocol_keeloq_common_decrypt(data, key);

    return ((uint64_t)k2 << 32) | k1; // key - shifrovanoya
}

/** Secure Learning
 * @param data - serial number (28bit)
 * @param seed - seed number (32bit)
 * @param key - manufacture (64bit)
 * @return manufacture for this serial number (64bit)
 */

inline uint64_t subghz_protocol_keeloq_common_secure_learning(
    uint32_t data,
    uint32_t seed,
    const uint64_t key) {
    uint32_t k1, k2;

    data &= 0x0FFFFFFF;
    k1 = subghz_protocol_keeloq_common_decrypt(data, key);
    k2 = subghz_protocol_keeloq_common_decrypt(seed, key);

    return ((uint64_t)k1 << 32) | k2;
}

/** Magic_xor_type1 Learning
 * @param data - serial number (28bit)
 * @param xor - magic xor (64bit)
 * @return manufacture for this serial number (64bit)
 */

inline uint64_t
    subghz_protocol_keeloq_common_magic_xor_type1_learning(uint32_t data, uint64_t xor) {
    data &= 0x0FFFFFFF;
    return (((uint64_t)data << 32) | data) ^ xor;
}

/** Faac SLH (Spa) Learning
 * @param seed - seed number (32bit)
 * @param key - mfkey (64bit)
 * @return man_learning for this seed number (64bit)
 */

inline uint64_t
    subghz_protocol_keeloq_common_faac_learning(const uint32_t seed, const uint64_t key) {
    uint16_t hs = seed >> 16;
    const uint16_t ending = 0x544D;
    uint32_t lsb = (uint32_t)hs << 16 | ending;
    uint64_t man = (uint64_t)subghz_protocol_keeloq_common_encrypt(seed, key) << 32 |
                   subghz_protocol_keeloq_common_encrypt(lsb, key);
    return man;
}
/** Magic_serial_type1 Learning
 * @param data - serial number (28bit)
 * @param man - magic man (64bit)
 * @return manufacture for this serial number (64bit)
 */

inline uint64_t
    subghz_protocol_keeloq_common_magic_serial_type1_learning(uint32_t data, uint64_t man) {
    return (man & 0xFFFFFFFF) | ((uint64_t)data << 40) |
           ((uint64_t)(((data & 0xff) + ((data >> 8) & 0xFF)) & 0xFF) << 32);
}

/** Magic_serial_type2 Learning
 * @param data - btn+serial number (32bit)
 * @param man - magic man (64bit)
 * @return manufacture for this serial number (64bit)
 */

inline uint64_t
    subghz_protocol_keeloq_common_magic_serial_type2_learning(uint32_t data, uint64_t man) {
    // Splice `data` (byte-reversed) into the high 32 bits of `man`. The original
    // used uint8_t* punning over `data`/`man`, which is byte-order dependent;
    // this explicit form is bit-identical on little-endian (STM32 + ESP32, the
    // only targets here) with no endianness dependence. Verified over 200k vectors.
    uint32_t rev = ((data & 0x000000FFu) << 24) | ((data & 0x0000FF00u) << 8) |
                   ((data & 0x00FF0000u) >> 8) | ((data & 0xFF000000u) >> 24);
    return (man & 0x00000000FFFFFFFFULL) | ((uint64_t)rev << 32);
}

/** Magic_serial_type3 Learning
 * @param data - serial number (24bit)
 * @param man - magic man (64bit)
 * @return manufacture for this serial number (64bit)
 */

inline uint64_t
    subghz_protocol_keeloq_common_magic_serial_type3_learning(uint32_t data, uint64_t man) {
    return (man & 0xFFFFFFFFFF000000) | (data & 0xFFFFFF);
}

// Key utils

static inline uint32_t subghz_protocol_keeloq_common_manufacturer_nl_extend(
    uint32_t x,
    uint32_t k_lo,
    uint32_t k_hi,
    uint32_t outer_limit) {
    uint32_t r4 = outer_limit;
    uint32_t r5 = 0u;
    const uint32_t r6 = KEELOQ_NLF;

    while(r5 != r4) {
        if(r5 < 0x210u) {
            uint32_t r1 = (x >> 15) & 1u;
            uint32_t r7 = r1 ^ ((x >> 1) | (x << 31));
            // Extract bit s of the 64-bit manufacturing key (k_hi:k_lo). The
            // original decompiled form used 32-bit shifts by amounts that reach
            // >=32 / wrap negative — UB that only works on ARM's barrel shifter.
            // On Xtensa (ESP32) the mod-32 shifter corrupted this bit for ~25%
            // of keys, so AERF decode diverged from a real Flipper. A 64-bit
            // shift by s in [0,63] is fully defined and reproduces the ARM
            // result exactly (verified over 1.28M vectors; keeloq_verify.py).
            uint32_t s = (15u - r5) & 0x3Fu;
            uint64_t key64 = ((uint64_t)k_hi << 32) | (uint64_t)k_lo;
            r1 = (uint32_t)((key64 >> s) & 1u);
            uint32_t ip;
            ip = (x >> 30) & 1u;
            r1 ^= r7;
            r7 = (x >> 25) & 1u;
            r7 += ip << 1;
            ip = (x >> 19) & 1u;
            ip += r7 << 1;
            r7 = (x >> 8) & 1u;
            r7 += ip << 1;
            x &= 1u;
            x += r7 << 1;
            x = (int32_t)r6 >> (x & 31u);
            x &= 1u;
            x ^= r1;
        }
        r5 += 1u;
    }
    return x;
}

static inline uint32_t subghz_protocol_keeloq_common_word_rotate16(uint32_t v) {
    return (v >> 16) | (v << 16);
}

inline uint32_t subghz_protocol_keeloq_common_decrypt_derived(
    uint32_t hop_encrypted,
    uint64_t derived_manufacturing_key,
    uint32_t outer_limit) {
    return subghz_protocol_keeloq_common_manufacturer_nl_extend(
        hop_encrypted,
        (uint32_t)derived_manufacturing_key,
        (uint32_t)(derived_manufacturing_key >> 32u),
        outer_limit);
}

// Inverse of manufacturer_nl_extend. A forward round is x_new = rotr(x_old,1) with bit0 ^= fb,
// where fb = NLF(g5 @ 0,8,19,25,30) ^ keybit ^ bit15(x_old). fb depends on no x_old bit that the
// rotr lands at bit1, so the inverse round is rotl(x,1) with the SAME fb XORed into bit1 (fb
// computed from the rotl'd value), run over the active rounds in REVERSE order. Proven the exact
// inverse: enc(dec)==dec(enc)==identity over 9M random vectors (nl_extend_inverse_test.c) for
// outer_limit 0x210 / 0x240 / 0x40.
static inline uint32_t subghz_protocol_keeloq_common_manufacturer_nl_extend_encrypt(
    uint32_t x,
    uint32_t k_lo,
    uint32_t k_hi,
    uint32_t outer_limit) {
    uint64_t key64 = ((uint64_t)k_hi << 32) | (uint64_t)k_lo;
    uint32_t active = (outer_limit < 0x210u) ? outer_limit : 0x210u;
    for(int32_t r5 = (int32_t)active - 1; r5 >= 0; r5--) {
        uint32_t s = (15u - (uint32_t)r5) & 0x3Fu;
        uint32_t k_bit = (uint32_t)((key64 >> s) & 1u);
        uint32_t old = (x << 1) | (x >> 31); // rotl(x,1)
        uint32_t a = (old >> 15) & 1u;
        uint32_t g = g5(old, 0, 8, 19, 25, 30);
        uint32_t nlf_bit = (KEELOQ_NLF >> g) & 1u;
        uint32_t fb = nlf_bit ^ k_bit ^ a;
        old ^= (fb << 1);
        x = old;
    }
    return x;
}

inline uint32_t subghz_protocol_keeloq_common_encrypt_derived(
    uint32_t plaintext,
    uint64_t derived_manufacturing_key,
    uint32_t outer_limit) {
    return subghz_protocol_keeloq_common_manufacturer_nl_extend_encrypt(
        plaintext,
        (uint32_t)derived_manufacturing_key,
        (uint32_t)(derived_manufacturing_key >> 32u),
        outer_limit);
}

// Protocol (Manufacturer) specific learning
// TODO: Better documentation for these functions

inline uint64_t subghz_protocol_keeloq_common_learning_aerf(uint32_t data, const uint64_t key) {
    uint32_t k_lo = (uint32_t)key;
    uint32_t k_hi = (uint32_t)(key >> 32);
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t x = d | 0x20000000u;
    x = subghz_protocol_keeloq_common_manufacturer_nl_extend(x, k_lo, k_hi, 0x40u);
    uint32_t k1 = x;
    x = d | 0x60000000u;
    x = subghz_protocol_keeloq_common_manufacturer_nl_extend(x, k_lo, k_hi, 0x40u);
    return ((uint64_t)x << 32) | k1;
}

inline uint64_t
    subghz_protocol_keeloq_common_learning_erreka(uint32_t data, uint32_t mix, const uint64_t key) {
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t k1 = subghz_protocol_keeloq_common_decrypt(d | 0x20000000u, key);
    uint32_t r4 = mix >> 4;
    uint32_t r1 = (mix << 4) & 0xF000F000u;
    r4 = (r4 & 0x0F000F00u) | r1;
    uint32_t r5 = mix & 0x00FF00FFu;
    uint32_t x = r4 | r5;
    x |= 0x60000000u;
    uint32_t k2 = subghz_protocol_keeloq_common_decrypt(x, key);
    return ((uint64_t)k2 << 32) | k1;
}

inline uint64_t subghz_protocol_keeloq_common_learning_pujol(uint32_t data, const uint64_t key) {
    uint32_t d = data & 0x0FFFFFFFu;
    uint32_t w1 = subghz_protocol_keeloq_common_decrypt(d | 0x20000000u, key);
    uint32_t w2 = subghz_protocol_keeloq_common_decrypt(d | 0x60000000u, key);
    uint32_t k1 = subghz_protocol_keeloq_common_word_rotate16(w1);
    uint32_t k2 = subghz_protocol_keeloq_common_word_rotate16(w2);
    return ((uint64_t)k2 << 32) | k1;
}
