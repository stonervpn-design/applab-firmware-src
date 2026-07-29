#include "net_ntlm.h"
#include <ctype.h>

/* ============ MD5 Implementation (RFC 1321) ============ */

#define MD5_BLOCK_SIZE 64
#define MD5_DIGEST_SIZE 16

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[MD5_BLOCK_SIZE];
} MD5_CTX;

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint32_t md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

#define ROTLEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define LEFTROTATE(x, n) ROTLEFT(x, n)

static void md5_transform(MD5_CTX* ctx, const uint8_t data[MD5_BLOCK_SIZE]) {
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t x[16];

    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        x[i] = (uint32_t)(data[j] | (data[j+1] << 8) | (data[j+2] << 16) | (data[j+3] << 24));
    }

    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;

        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | (~d));
            g = (7 * i) % 16;
        }

        uint32_t temp = d;
        d = c;
        c = b;
        b = b + LEFTROTATE(a + f + md5_k[i] + x[g], md5_r[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md5_init(MD5_CTX* ctx) {
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(MD5_CTX* ctx, const uint8_t* input, size_t len) {
    uint32_t index = (ctx->count[0] >> 3) & 0x3f;
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t partlen = 64 - index;
    size_t i = 0;

    if (len >= partlen) {
        memcpy(&ctx->buffer[index], input, partlen);
        md5_transform(ctx, ctx->buffer);
        for (i = partlen; i + 63 < len; i += 64) {
            md5_transform(ctx, &input[i]);
        }
        index = 0;
    }

    memcpy(&ctx->buffer[index], &input[i], len - i);
}

static void md5_final(MD5_CTX* ctx, uint8_t digest[MD5_DIGEST_SIZE]) {
    uint32_t index = (ctx->count[0] >> 3) & 0x3f;
    uint32_t padlen = (index < 56) ? (56 - index) : (120 - index);

    uint8_t bits[8];
    for (int i = 0; i < 4; ++i) {
        bits[i] = (ctx->count[0] >> (i * 8)) & 0xff;
        bits[i+4] = (ctx->count[1] >> (i * 8)) & 0xff;
    }

    uint8_t padding[64] = {0x80};
    md5_update(ctx, padding, padlen);
    md5_update(ctx, bits, 8);

    for (int i = 0; i < 4; ++i) {
        digest[i] = (ctx->state[0] >> (i * 8)) & 0xff;
        digest[i+4] = (ctx->state[1] >> (i * 8)) & 0xff;
        digest[i+8] = (ctx->state[2] >> (i * 8)) & 0xff;
        digest[i+12] = (ctx->state[3] >> (i * 8)) & 0xff;
    }
}

static void md5_hash(const uint8_t* input, size_t len, uint8_t digest[MD5_DIGEST_SIZE]) {
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, input, len);
    md5_final(&ctx, digest);
}

/* ============ HMAC-MD5 Implementation (RFC 2104) ============ */

static void hmac_md5(const uint8_t* key, size_t key_len,
                      const uint8_t* data, size_t data_len,
                      uint8_t out[MD5_DIGEST_SIZE]) {
    uint8_t ipad[MD5_BLOCK_SIZE], opad[MD5_BLOCK_SIZE];
    uint8_t temp[MD5_DIGEST_SIZE];

    // Normalize key
    uint8_t normalized_key[MD5_BLOCK_SIZE] = {0};
    if (key_len > MD5_BLOCK_SIZE) {
        md5_hash(key, key_len, normalized_key);
    } else {
        memcpy(normalized_key, key, key_len);
    }

    // Compute ipad and opad
    for (int i = 0; i < MD5_BLOCK_SIZE; ++i) {
        ipad[i] = normalized_key[i] ^ 0x36;
        opad[i] = normalized_key[i] ^ 0x5c;
    }

    // Inner hash
    MD5_CTX ctx;
    md5_init(&ctx);
    md5_update(&ctx, ipad, MD5_BLOCK_SIZE);
    md5_update(&ctx, data, data_len);
    md5_final(&ctx, temp);

    // Outer hash
    md5_init(&ctx);
    md5_update(&ctx, opad, MD5_BLOCK_SIZE);
    md5_update(&ctx, temp, MD5_DIGEST_SIZE);
    md5_final(&ctx, out);
}

/* ============ MD4 Implementation (simplified for NTLM) ============ */

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} MD4_CTX;

#define MD4_F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define MD4_G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define MD4_H(x, y, z) ((x) ^ (y) ^ (z))

static void md4_transform(MD4_CTX* ctx, const uint8_t data[64]) {
    uint32_t x[16];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        x[i] = (uint32_t)(data[j] | (data[j+1] << 8) | (data[j+2] << 16) | (data[j+3] << 24));
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];

    // Round 1
    for (int i = 0; i < 16; i += 4) {
        a = (a + MD4_F(b, c, d) + x[i]) & 0xffffffff; a = LEFTROTATE(a, 3);
        d = (d + MD4_F(a, b, c) + x[i+1]) & 0xffffffff; d = LEFTROTATE(d, 7);
        c = (c + MD4_F(d, a, b) + x[i+2]) & 0xffffffff; c = LEFTROTATE(c, 11);
        b = (b + MD4_F(c, d, a) + x[i+3]) & 0xffffffff; b = LEFTROTATE(b, 19);
    }

    // Round 2
    for (int i = 0; i < 16; i += 4) {
        a = (a + MD4_G(b, c, d) + x[i] + 0x5a827999) & 0xffffffff; a = LEFTROTATE(a, 3);
        d = (d + MD4_G(a, b, c) + x[i+1] + 0x5a827999) & 0xffffffff; d = LEFTROTATE(d, 5);
        c = (c + MD4_G(d, a, b) + x[i+2] + 0x5a827999) & 0xffffffff; c = LEFTROTATE(c, 9);
        b = (b + MD4_G(c, d, a) + x[i+3] + 0x5a827999) & 0xffffffff; b = LEFTROTATE(b, 13);
    }

    // Round 3
    int indices_r3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (int i = 0; i < 16; i += 4) {
        a = (a + MD4_H(b, c, d) + x[indices_r3[i]] + 0x6ed9eba1) & 0xffffffff; a = LEFTROTATE(a, 3);
        d = (d + MD4_H(a, b, c) + x[indices_r3[i+1]] + 0x6ed9eba1) & 0xffffffff; d = LEFTROTATE(d, 9);
        c = (c + MD4_H(d, a, b) + x[indices_r3[i+2]] + 0x6ed9eba1) & 0xffffffff; c = LEFTROTATE(c, 11);
        b = (b + MD4_H(c, d, a) + x[indices_r3[i+3]] + 0x6ed9eba1) & 0xffffffff; b = LEFTROTATE(b, 15);
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

static void md4_init(MD4_CTX* ctx) {
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md4_update(MD4_CTX* ctx, const uint8_t* input, size_t len) {
    uint32_t index = (ctx->count[0] >> 3) & 0x3f;
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t partlen = 64 - index;
    size_t i = 0;

    if (len >= partlen) {
        memcpy(&ctx->buffer[index], input, partlen);
        md4_transform(ctx, ctx->buffer);
        for (i = partlen; i + 63 < len; i += 64) {
            md4_transform(ctx, &input[i]);
        }
        index = 0;
    }

    memcpy(&ctx->buffer[index], &input[i], len - i);
}

static void md4_final(MD4_CTX* ctx, uint8_t digest[16]) {
    uint32_t index = (ctx->count[0] >> 3) & 0x3f;
    uint32_t padlen = (index < 56) ? (56 - index) : (120 - index);

    uint8_t bits[8];
    for (int i = 0; i < 4; ++i) {
        bits[i] = (ctx->count[0] >> (i * 8)) & 0xff;
        bits[i+4] = (ctx->count[1] >> (i * 8)) & 0xff;
    }

    uint8_t padding[64] = {0x80};
    md4_update(ctx, padding, padlen);
    md4_update(ctx, bits, 8);

    for (int i = 0; i < 4; ++i) {
        digest[i] = (ctx->state[0] >> (i * 8)) & 0xff;
        digest[i+4] = (ctx->state[1] >> (i * 8)) & 0xff;
        digest[i+8] = (ctx->state[2] >> (i * 8)) & 0xff;
        digest[i+12] = (ctx->state[3] >> (i * 8)) & 0xff;
    }
}

static void md4_hash(const uint8_t* input, size_t len, uint8_t digest[16]) {
    MD4_CTX ctx;
    md4_init(&ctx);
    md4_update(&ctx, input, len);
    md4_final(&ctx, digest);
}

/* ============ UTF-16LE Encoding ============ */

static size_t utf16le_encode(const char* src, uint8_t* dst, size_t dst_max) {
    size_t len = 0;
    for (int i = 0; src[i] && len + 1 < dst_max; ++i) {
        uint8_t c = (uint8_t)src[i];
        dst[len++] = c;
        dst[len++] = 0;
    }
    return len;
}

/* ============ NTLMv2 Response Computation ============ */

void ntlmv2_response_compute(const char* password, const char* username, const char* domain,
                              const uint8_t challenge[8], const uint8_t context[8],
                              uint8_t out_response[16]) {
    // Step 1: Compute NT hash = MD4(UTF16-LE(password))
    uint8_t pwd_utf16le[512];
    size_t pwd_len = utf16le_encode(password, pwd_utf16le, sizeof(pwd_utf16le));
    uint8_t nt_hash[16];
    md4_hash(pwd_utf16le, pwd_len, nt_hash);

    // Step 2: Compute NTLMv2 hash = HMAC-MD5(NT-hash, UPPER(domain) || user)
    // Prepare domain and username
    size_t i;
    char domain_upper[256] = {0};
    for (i = 0; domain[i] && i < sizeof(domain_upper)-1; ++i) {
        domain_upper[i] = toupper((unsigned char)domain[i]);
    }
    domain_upper[i] = '\0';

    uint8_t identity[512];
    size_t identity_len = 0;

    // Encode domain in UTF-16LE (cap so the 2-byte-per-char writes can never
    // exceed identity[]; each iteration needs 2 free bytes).
    for (i = 0; domain_upper[i] && identity_len + 2 <= sizeof(identity); ++i) {
        identity[identity_len++] = (uint8_t)domain_upper[i];
        identity[identity_len++] = 0;
    }

    // Encode username in UTF-16LE (same cap; username is otherwise unbounded).
    for (i = 0; username[i] && identity_len + 2 <= sizeof(identity); ++i) {
        identity[identity_len++] = (uint8_t)username[i];
        identity[identity_len++] = 0;
    }

    uint8_t ntlmv2_hash[16];
    hmac_md5(nt_hash, 16, identity, identity_len, ntlmv2_hash);

    // Step 3: Compute response = HMAC-MD5(NTLMv2-hash, challenge || context)
    uint8_t challenge_blob[16];
    memcpy(&challenge_blob[0], challenge, 8);
    memcpy(&challenge_blob[8], context, 8);

    hmac_md5(ntlmv2_hash, 16, challenge_blob, 16, out_response);
}

/* ============ NTLM Type-3 Message Parser ============ */

bool ntlm_parse_type3(const uint8_t* frame, size_t len, NtlmV2Parsed* out) {
    if (!frame || !out || len < 56) {
        return false;  // Type-3 messages are at least 56 bytes
    }

    // Signature check: "NTLMSSP\0"
    if (frame[0] != 'N' || frame[1] != 'T' || frame[2] != 'L' || frame[3] != 'M' ||
        frame[4] != 'S' || frame[5] != 'S' || frame[6] != 'P' || frame[7] != 0) {
        return false;
    }

    // Message type (offset 8)
    if (frame[8] != 3) {
        return false;  // Not a Type-3 message
    }

    // Extract field offsets and lengths from Type-3 message
    // Descriptor format: uint16 length + uint16 maxlength + uint32 offset
    // Structure:
    // 0-7: Signature
    // 8-11: Type (3)
    // 12-19: LmChallengeResponse descriptor
    // 20-27: NtChallengeResponse descriptor (this is what we want)
    // 28-35: Domain descriptor
    // 36-43: Username descriptor
    // 44-51: Workstation descriptor

    // Read NtChallengeResponse descriptor (offset 20-27)
    if (len < 28) return false;
    uint16_t nt_len = frame[20] | (frame[21] << 8);
    uint32_t nt_offset = frame[24] | (frame[25] << 8) | (frame[26] << 16) | (frame[27] << 24);

    // Read Domain descriptor (offset 28-35)
    if (len < 36) return false;
    uint16_t dom_len = frame[28] | (frame[29] << 8);
    uint32_t dom_offset = frame[32] | (frame[33] << 8) | (frame[34] << 16) | (frame[35] << 24);

    // Read Username descriptor (offset 36-43)
    if (len < 44) return false;
    uint16_t user_len = frame[36] | (frame[37] << 8);
    uint32_t user_offset = frame[40] | (frame[41] << 8) | (frame[42] << 16) | (frame[43] << 24);

    // Bounds check all fields. Validate each (offset,len) pair without any
    // addition that could 32-bit wrap (nt_offset+nt_len overflowing would
    // bypass the check and allow a wild memcpy below). len is size_t; each
    // length is verified <= len first, so the len-<field>_len subtraction
    // cannot underflow.
    if (nt_len > len || nt_offset > len - nt_len ||
        dom_len > len || dom_offset > len - dom_len ||
        user_len > len || user_offset > len - user_len) {
        return false;
    }

    // NtChallengeResponse should be at least 24 bytes (8 byte server challenge + 16 byte HMAC-MD5)
    if (nt_len < 24) {
        return false;
    }

    // Extract challenge (first 8 bytes of response in basic auth, but in v2 it's in the blob)
    // For simplified parsing: assume the challenge is provided externally or extract from context
    // Here we'll zero it and note that real implementations need the server challenge
    memset(out->challenge, 0, 8);

    // Extract response (16 bytes HMAC-MD5)
    if (nt_len >= 16) {
        memcpy(out->response, &frame[nt_offset], 16);
    } else {
        return false;
    }

    // Extract domain (UTF-16LE)
    size_t dom_chars = dom_len / 2;
    if (dom_chars >= sizeof(out->domain)) dom_chars = sizeof(out->domain) - 1;
    for (size_t i = 0; i < dom_chars; ++i) {
        out->domain[i] = frame[dom_offset + i*2];
    }
    out->domain[dom_chars] = '\0';

    // Extract username (UTF-16LE)
    size_t user_chars = user_len / 2;
    if (user_chars >= sizeof(out->username)) user_chars = sizeof(out->username) - 1;
    for (size_t i = 0; i < user_chars; ++i) {
        out->username[i] = frame[user_offset + i*2];
    }
    out->username[user_chars] = '\0';

    // Ensure null termination
    out->domain[sizeof(out->domain)-1] = '\0';
    out->username[sizeof(out->username)-1] = '\0';

    return true;
}

/* ============ Hashcat 5500 Formatter ============ */

static void bytes_to_hex(const uint8_t* src, size_t len, char* dst) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        dst[i*2] = hex[(src[i] >> 4) & 0xf];
        dst[i*2+1] = hex[src[i] & 0xf];
    }
    dst[len*2] = '\0';
}

size_t net_ntlm_hashcat_5500(const NtlmV2Parsed* parsed, char* out, size_t out_max) {
    if (!parsed || !out || out_max == 0) {
        return 0;
    }

    // Format: user::domain:serverchallenge:NTProofStr:blob
    // For simplified version: user::domain:0000000000000000:NTProofStr:blob

    size_t written = 0;

    // Username
    size_t user_len = strlen(parsed->username);
    if (written + user_len + 2 >= out_max) return 0;  // +2 for "::"
    memcpy(&out[written], parsed->username, user_len);
    written += user_len;
    out[written++] = ':';
    out[written++] = ':';

    // Domain
    size_t dom_len = strlen(parsed->domain);
    if (written + dom_len + 1 >= out_max) return 0;  // +1 for ":"
    memcpy(&out[written], parsed->domain, dom_len);
    written += dom_len;
    out[written++] = ':';

    // Server challenge (as hex, 8 bytes = 16 hex chars)
    if (written + 16 + 1 >= out_max) return 0;  // +1 for ":"
    bytes_to_hex(parsed->challenge, 8, &out[written]);
    written += 16;
    out[written++] = ':';

    // NTProofStr (16 bytes of the response = 32 hex chars)
    if (written + 32 + 1 >= out_max) return 0;  // +1 for ":"
    bytes_to_hex(parsed->response, 16, &out[written]);
    written += 32;
    out[written++] = ':';

    // Blob (for now, empty; real implementations would extract from full response)
    // In a full implementation, this would be any additional context bytes

    out[written] = '\0';
    return written;
}
