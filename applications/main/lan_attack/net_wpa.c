#include "net_wpa.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ============ SHA-1 Implementation (RFC 3174) ============ */

#define SHA1_BLOCK_SIZE 64
#define SHA1_DIGEST_SIZE 20

typedef struct {
    uint32_t H[5];
    uint32_t W[80];
    unsigned char MessageBlock[64];
    int MessageBlockIndex;
    uint32_t LengthHigh;
    uint32_t LengthLow;
} SHA1Context;

static uint32_t circular_shift(int bits, uint32_t word) {
    return ((word << bits) | (word >> (32 - bits)));
}

static void SHA1ProcessMessageBlock(SHA1Context *context) {
    uint32_t t, temp;
    int i;

    for (t = 0; t < 16; t++)
        context->W[t] = ((uint32_t)context->MessageBlock[t * 4] << 24) |
                        ((uint32_t)context->MessageBlock[t * 4 + 1] << 16) |
                        ((uint32_t)context->MessageBlock[t * 4 + 2] << 8) |
                        ((uint32_t)context->MessageBlock[t * 4 + 3]);

    for (t = 16; t < 80; t++)
        context->W[t] = circular_shift(1, context->W[t - 3] ^ context->W[t - 8] ^
                                            context->W[t - 14] ^ context->W[t - 16]);

    uint32_t A = context->H[0];
    uint32_t B = context->H[1];
    uint32_t C = context->H[2];
    uint32_t D = context->H[3];
    uint32_t E = context->H[4];

    for (t = 0; t < 20; t++) {
        temp = (circular_shift(5, A) + ((B & C) | ((~B) & D)) + E + 0x5A827999 + context->W[t]) & 0xFFFFFFFF;
        E = D;
        D = C;
        C = circular_shift(30, B);
        B = A;
        A = temp;
    }

    for (t = 20; t < 40; t++) {
        temp = (circular_shift(5, A) + (B ^ C ^ D) + E + 0x6ED9EBA1 + context->W[t]) & 0xFFFFFFFF;
        E = D;
        D = C;
        C = circular_shift(30, B);
        B = A;
        A = temp;
    }

    for (t = 40; t < 60; t++) {
        temp = (circular_shift(5, A) + ((B & C) | (B & D) | (C & D)) + E + 0x8F1BBCDC + context->W[t]) & 0xFFFFFFFF;
        E = D;
        D = C;
        C = circular_shift(30, B);
        B = A;
        A = temp;
    }

    for (t = 60; t < 80; t++) {
        temp = (circular_shift(5, A) + (B ^ C ^ D) + E + 0xCA62C1D6 + context->W[t]) & 0xFFFFFFFF;
        E = D;
        D = C;
        C = circular_shift(30, B);
        B = A;
        A = temp;
    }

    context->H[0] = (context->H[0] + A) & 0xFFFFFFFF;
    context->H[1] = (context->H[1] + B) & 0xFFFFFFFF;
    context->H[2] = (context->H[2] + C) & 0xFFFFFFFF;
    context->H[3] = (context->H[3] + D) & 0xFFFFFFFF;
    context->H[4] = (context->H[4] + E) & 0xFFFFFFFF;

    context->MessageBlockIndex = 0;
}

static void SHA1Reset(SHA1Context *context) {
    context->LengthHigh = 0;
    context->LengthLow = 0;
    context->MessageBlockIndex = 0;

    context->H[0] = 0x67452301;
    context->H[1] = 0xEFCDAB89;
    context->H[2] = 0x98BADCFE;
    context->H[3] = 0x10325476;
    context->H[4] = 0xC3D2E1F0;
}

static void SHA1Input(SHA1Context *context, const unsigned char *message_array, unsigned int length) {
    if (!message_array || length == 0)
        return;

    for (unsigned int i = 0; i < length; i++) {
        context->MessageBlock[context->MessageBlockIndex++] = message_array[i];

        context->LengthLow += 8;
        if (context->LengthLow == 0) {
            context->LengthHigh++;
        }

        if (context->MessageBlockIndex == 64) {
            SHA1ProcessMessageBlock(context);
        }
    }
}

static void SHA1Result(SHA1Context *context, unsigned char Message_Digest[20]) {
    if (context->MessageBlockIndex > 55) {
        context->MessageBlock[context->MessageBlockIndex++] = 0x80;
        while (context->MessageBlockIndex < 64) {
            context->MessageBlock[context->MessageBlockIndex++] = 0;
        }
        SHA1ProcessMessageBlock(context);

        while (context->MessageBlockIndex < 56) {
            context->MessageBlock[context->MessageBlockIndex++] = 0;
        }
    } else {
        context->MessageBlock[context->MessageBlockIndex++] = 0x80;
        while (context->MessageBlockIndex < 56) {
            context->MessageBlock[context->MessageBlockIndex++] = 0;
        }
    }

    context->MessageBlock[56] = context->LengthHigh >> 24;
    context->MessageBlock[57] = context->LengthHigh >> 16;
    context->MessageBlock[58] = context->LengthHigh >> 8;
    context->MessageBlock[59] = context->LengthHigh;
    context->MessageBlock[60] = context->LengthLow >> 24;
    context->MessageBlock[61] = context->LengthLow >> 16;
    context->MessageBlock[62] = context->LengthLow >> 8;
    context->MessageBlock[63] = context->LengthLow;

    SHA1ProcessMessageBlock(context);

    for (int i = 0; i < 20; i++) {
        Message_Digest[i] = context->H[i >> 2] >> (8 * (3 - (i % 4)));
    }
}

/* ============ MD5 Implementation (RFC 1321) ============ */

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    unsigned char buffer[64];
} MD5Context;

#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define FF(a, b, c, d, x, s, ac)                    \
    {                                                 \
        (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = ROTATE_LEFT((a), (s));                 \
        (a) += (b);                                  \
    }
#define GG(a, b, c, d, x, s, ac)                    \
    {                                                 \
        (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = ROTATE_LEFT((a), (s));                 \
        (a) += (b);                                  \
    }
#define HH(a, b, c, d, x, s, ac)                    \
    {                                                 \
        (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = ROTATE_LEFT((a), (s));                 \
        (a) += (b);                                  \
    }
#define II(a, b, c, d, x, s, ac)                    \
    {                                                 \
        (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = ROTATE_LEFT((a), (s));                 \
        (a) += (b);                                  \
    }

static void MD5Transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];

    for (int i = 0, j = 0; j < 64; i++, j += 4) {
        x[i] = ((uint32_t)block[j]) | (((uint32_t)block[j + 1]) << 8) |
               (((uint32_t)block[j + 2]) << 16) | (((uint32_t)block[j + 3]) << 24);
    }

    FF(a, b, c, d, x[0], 7, 0xd76aa478);
    FF(d, a, b, c, x[1], 12, 0xe8c7b756);
    FF(c, d, a, b, x[2], 17, 0x242070db);
    FF(b, c, d, a, x[3], 22, 0xc1bdceee);
    FF(a, b, c, d, x[4], 7, 0xf57c0faf);
    FF(d, a, b, c, x[5], 12, 0x4787c62a);
    FF(c, d, a, b, x[6], 17, 0xa8304613);
    FF(b, c, d, a, x[7], 22, 0xfd469501);
    FF(a, b, c, d, x[8], 7, 0x698098d8);
    FF(d, a, b, c, x[9], 12, 0x8b44f7af);
    FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    FF(b, c, d, a, x[11], 22, 0x895cd7be);
    FF(a, b, c, d, x[12], 7, 0x6b901122);
    FF(d, a, b, c, x[13], 12, 0xfd987193);
    FF(c, d, a, b, x[14], 17, 0xa8e48a64);
    FF(b, c, d, a, x[15], 22, 0xe9b6c7aa);

    GG(a, b, c, d, x[1], 5, 0xf61e2562);
    GG(d, a, b, c, x[6], 9, 0xc040b340);
    GG(c, d, a, b, x[11], 14, 0x265e5a51);
    GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, x[5], 5, 0xd62f105d);
    GG(d, a, b, c, x[10], 9, 0x02441453);
    GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, x[9], 5, 0x21e1cde6);
    GG(d, a, b, c, x[14], 9, 0xc33707d6);
    GG(c, d, a, b, x[3], 14, 0xf4d50d87);
    GG(b, c, d, a, x[8], 20, 0x455a14ed);
    GG(a, b, c, d, x[13], 5, 0xa9e3e905);
    GG(d, a, b, c, x[2], 9, 0xfcefa3f8);
    GG(c, d, a, b, x[7], 14, 0x676f02d9);
    GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    HH(a, b, c, d, x[5], 4, 0xfffa3942);
    HH(d, a, b, c, x[8], 11, 0x8771f681);
    HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    HH(b, c, d, a, x[14], 23, 0xfde5380c);
    HH(a, b, c, d, x[1], 4, 0xa4beea44);
    HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
    HH(c, d, a, b, x[7], 16, 0xf6bb4b60);
    HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    HH(a, b, c, d, x[13], 4, 0x289b7ec6);
    HH(d, a, b, c, x[0], 11, 0xeaa127fa);
    HH(c, d, a, b, x[3], 16, 0xd4ef3085);
    HH(b, c, d, a, x[6], 23, 0x04881d05);
    HH(a, b, c, d, x[9], 4, 0xd9d4d039);
    HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    HH(b, c, d, a, x[2], 23, 0xc4ac5665);

    II(a, b, c, d, x[0], 6, 0xf4292244);
    II(d, a, b, c, x[7], 10, 0x432aff97);
    II(c, d, a, b, x[14], 15, 0xab9423a7);
    II(b, c, d, a, x[5], 21, 0xfc93a039);
    II(a, b, c, d, x[12], 6, 0x655b59c3);
    II(d, a, b, c, x[3], 10, 0x8f0ccc92);
    II(c, d, a, b, x[10], 15, 0xffeff47d);
    II(b, c, d, a, x[1], 21, 0x85845dd1);
    II(a, b, c, d, x[8], 6, 0x6fa87e4f);
    II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, x[6], 15, 0xa3014314);
    II(b, c, d, a, x[13], 21, 0x4e0811a1);
    II(a, b, c, d, x[4], 6, 0xf7537e82);
    II(d, a, b, c, x[11], 10, 0xbd3af235);
    II(c, d, a, b, x[2], 15, 0x2ad7d2bb);
    II(b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void MD5Init(MD5Context *context) {
    context->count[0] = 0;
    context->count[1] = 0;
    context->state[0] = 0x67452301;
    context->state[1] = 0xefcdab89;
    context->state[2] = 0x98badcfe;
    context->state[3] = 0x10325476;
}

static void MD5Update(MD5Context *context, const unsigned char *input, unsigned int inputLen) {
    unsigned int index = (context->count[0] >> 3) & 0x3F;
    unsigned int inputIndex = 0;

    if ((context->count[0] += ((uint32_t)inputLen << 3)) < ((uint32_t)inputLen << 3))
        context->count[1]++;
    context->count[1] += (uint32_t)inputLen >> 29;

    unsigned int partLen = 64 - index;

    if (inputLen >= partLen) {
        memcpy(&context->buffer[index], &input[0], partLen);
        MD5Transform(context->state, context->buffer);

        for (inputIndex = partLen; inputIndex + 63 < inputLen; inputIndex += 64)
            MD5Transform(context->state, &input[inputIndex]);

        index = 0;
    } else {
        inputIndex = 0;
    }

    memcpy(&context->buffer[index], &input[inputIndex], inputLen - inputIndex);
}

static void MD5Final(unsigned char digest[16], MD5Context *context) {
    unsigned char bits[8];
    unsigned int index = (context->count[0] >> 3) & 0x3f;
    unsigned int padLen = (index < 56) ? (56 - index) : (120 - index);

    for (int i = 0; i < 4; i++) {
        bits[i] = (unsigned char)(context->count[0] >> (i * 8)) & 0xff;
        bits[i + 4] = (unsigned char)(context->count[1] >> (i * 8)) & 0xff;
    }

    unsigned char padding[64] = {0x80};
    MD5Update(context, padding, padLen);
    MD5Update(context, bits, 8);

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            digest[i * 4 + j] = (unsigned char)(context->state[i] >> (j * 8)) & 0xff;
        }
    }
}

/* ============ HMAC-SHA1 ============ */
static void hmac_sha1(const uint8_t *key, size_t keylen, const uint8_t *data,
                       size_t datalen, uint8_t out[20]) {
    SHA1Context ctx;
    uint8_t ipad[64], opad[64];
    int i;

    if (keylen > 64) {
        SHA1Reset(&ctx);
        SHA1Input(&ctx, key, keylen);
        SHA1Result(&ctx, out);
        key = out;
        keylen = 20;
    }

    for (i = 0; i < (int)keylen; i++) {
        ipad[i] = key[i] ^ 0x36;
        opad[i] = key[i] ^ 0x5c;
    }
    for (; i < 64; i++) {
        ipad[i] = 0x36;
        opad[i] = 0x5c;
    }

    SHA1Reset(&ctx);
    SHA1Input(&ctx, ipad, 64);
    SHA1Input(&ctx, data, datalen);
    SHA1Result(&ctx, out);

    SHA1Reset(&ctx);
    SHA1Input(&ctx, opad, 64);
    SHA1Input(&ctx, out, 20);
    SHA1Result(&ctx, out);
}

/* ============ HMAC-MD5 ============ */
static void hmac_md5(const uint8_t *key, size_t keylen, const uint8_t *data,
                      size_t datalen, uint8_t out[16]) {
    MD5Context ctx;
    uint8_t ipad[64], opad[64];
    int i;

    if (keylen > 64) {
        MD5Init(&ctx);
        MD5Update(&ctx, key, keylen);
        MD5Final(out, &ctx);
        key = out;
        keylen = 16;
    }

    for (i = 0; i < (int)keylen; i++) {
        ipad[i] = key[i] ^ 0x36;
        opad[i] = key[i] ^ 0x5c;
    }
    for (; i < 64; i++) {
        ipad[i] = 0x36;
        opad[i] = 0x5c;
    }

    MD5Init(&ctx);
    MD5Update(&ctx, ipad, 64);
    MD5Update(&ctx, data, datalen);
    MD5Final(out, &ctx);

    MD5Init(&ctx);
    MD5Update(&ctx, opad, 64);
    MD5Update(&ctx, out, 16);
    MD5Final(out, &ctx);
}

/* ============ PBKDF2-SHA1 (RFC 2898) ============ */
static void pbkdf2_sha1(const char *password, const uint8_t *salt, size_t saltlen,
                        int iterations, uint8_t *out, size_t outlen) {
    uint8_t *d = out;
    uint32_t hlen = 20; /* SHA1 output length */
    uint32_t l = (uint32_t)((outlen + hlen - 1) / hlen);
    uint32_t r = (uint32_t)(outlen - (l - 1) * hlen);
    uint8_t ti[20], u[20];
    uint32_t i, j;
    int c;

    size_t password_len = strlen(password);

    for (i = 1; i <= l; i++) {
        memset(ti, 0, hlen);

        uint8_t buffer[256];
        if (saltlen + 4 <= sizeof(buffer)) {
            memcpy(buffer, salt, saltlen);
            buffer[saltlen] = (i >> 24) & 0xff;
            buffer[saltlen + 1] = (i >> 16) & 0xff;
            buffer[saltlen + 2] = (i >> 8) & 0xff;
            buffer[saltlen + 3] = i & 0xff;
            hmac_sha1((const uint8_t *)password, password_len, buffer, saltlen + 4, u);
            memcpy(ti, u, hlen);
            for (c = 1; c < (uint32_t)iterations; c++) {
                hmac_sha1((const uint8_t *)password, password_len, u, hlen, u);
                for (j = 0; j < hlen; j++) {
                    ti[j] ^= u[j];
                }
            }
        }

        if (i < l) {
            memcpy(d, ti, hlen);
            d += hlen;
        } else {
            memcpy(d, ti, r);
        }
    }
}

/* ============ IEEE 802.11i PRF-SHA1 ============ */
static void prf_sha1(const uint8_t *key, size_t keylen, const char *label,
                      const uint8_t *data, size_t datalen, uint8_t *out,
                      size_t outlen) {
    uint8_t hmac_result[20];
    size_t hashlen = 20; /* SHA1 output length */
    uint32_t iterations = (uint32_t)((outlen + hashlen - 1) / hashlen);
    uint8_t counter = 1;
    size_t label_len = strlen(label);
    size_t offset = 0;

    uint8_t buffer[256];

    for (uint32_t i = 0; i < iterations; i++) {
        memcpy(buffer, &counter, 1);
        memcpy(buffer + 1, label, label_len);
        buffer[1 + label_len] = 0x00;
        memcpy(buffer + 1 + label_len + 1, data, datalen);

        hmac_sha1(key, keylen, buffer, 1 + label_len + 1 + datalen, hmac_result);

        size_t to_copy = (outlen - offset < hashlen) ? (outlen - offset) : hashlen;
        memcpy(out + offset, hmac_result, to_copy);
        offset += to_copy;
        counter++;
    }
}

/* ============ Public API ============ */

bool wpa_pmk_compute(const char *password, const char *ssid, size_t ssid_len,
                      uint8_t out_pmk[32]) {
    if (!password || !ssid || !out_pmk) {
        return false;
    }

    pbkdf2_sha1(password, (const uint8_t *)ssid, ssid_len, 4096, out_pmk, 32);
    return true;
}

bool wpa_ptk_compute(const uint8_t pmk[32], const uint8_t anonce[32],
                      const uint8_t snonce[32], const uint8_t ap_mac[6],
                      const uint8_t sta_mac[6], uint8_t out_ptk[48]) {
    if (!pmk || !anonce || !snonce || !ap_mac || !sta_mac || !out_ptk) {
        return false;
    }

    uint8_t prf_data[76];
    int cmp;

    cmp = memcmp(ap_mac, sta_mac, 6);
    if (cmp < 0) {
        memcpy(prf_data, ap_mac, 6);
        memcpy(prf_data + 6, sta_mac, 6);
    } else {
        memcpy(prf_data, sta_mac, 6);
        memcpy(prf_data + 6, ap_mac, 6);
    }

    cmp = memcmp(anonce, snonce, 32);
    if (cmp < 0) {
        memcpy(prf_data + 12, anonce, 32);
        memcpy(prf_data + 44, snonce, 32);
    } else {
        memcpy(prf_data + 12, snonce, 32);
        memcpy(prf_data + 44, anonce, 32);
    }

    prf_sha1(pmk, 32, "Pairwise key expansion", prf_data, 76, out_ptk, 48);
    return true;
}

bool wpa_mic_verify(const uint8_t ptk[48], const uint8_t eapol_frame[],
                     size_t eapol_len, const uint8_t captured_mic[16],
                     bool is_sha1) {
    if (!ptk || !eapol_frame || !captured_mic) {
        return false;
    }

    const uint8_t *kck = ptk;
    uint8_t computed_mic[16];

    if (is_sha1) {
        uint8_t mic_sha1[20];
        hmac_sha1(kck, 16, eapol_frame, eapol_len, mic_sha1);
        memcpy(computed_mic, mic_sha1, 16);
    } else {
        hmac_md5(kck, 16, eapol_frame, eapol_len, computed_mic);
    }

    return memcmp(computed_mic, captured_mic, 16) == 0;
}
