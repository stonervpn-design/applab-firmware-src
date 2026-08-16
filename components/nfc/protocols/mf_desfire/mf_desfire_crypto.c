// mf_desfire_crypto.c
#include "mf_desfire_crypto.h"
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/des.h>

size_t mf_desfire_key_size(MfDesfireKeyType t) {
    switch(t) {
    case MfDesfireKeyTypeDes: return 8;
    case MfDesfireKeyType2k3Des: return 16;
    case MfDesfireKeyType3k3Des: return 24;
    case MfDesfireKeyTypeAes: return 16;
    default: return 0;
    }
}
size_t mf_desfire_block_size(MfDesfireKeyType t) {
    // TRUE underlying CBC cipher block: AES = 16, all DES-family (DES/2K3DES/
    // 3K3DES) = 8, since 3K3DES is still 3DES (a 64-bit-block cipher) even
    // though its AuthenticateISO challenge is 16 bytes (see
    // mf_desfire_challenge_size). Task 5's CMAC keys off this field via
    // ctx->block_size to pick the Rb constant (0x1b for 8-byte block, 0x87
    // for 16) and to iterate/pad in cipher-block units — it must stay the
    // true cipher block, not the challenge length.
    return (t == MfDesfireKeyTypeAes) ? 16 : 8;
}
size_t mf_desfire_challenge_size(MfDesfireKeyType t) {
    // RndA/RndB (and thus token) length for the 3-pass authentication
    // handshake: AES and 3K3DES (AuthenticateISO, 0xAA/0x1A) use 16-byte
    // challenges; legacy DES/2K3DES (Authenticate, 0x0A) use 8-byte
    // challenges. For 3K3DES this differs from the true cipher block
    // (mf_desfire_block_size == 8): CBC still chains internally in 8-byte
    // DES sub-blocks (mbedtls handles this transparently for any length
    // that's a multiple of 8), but RndA/RndB/token/rotate/session-key
    // math all operate on the full 16 bytes. mf_desfire_derive_session_key's
    // 3K3DES case already reads offsets up to +12..+15, which only makes
    // sense against 16-byte RndA/RndB — this is the matching challenge length.
    return (t == MfDesfireKeyTypeAes || t == MfDesfireKeyType3k3Des) ? 16 : 8;
}
uint8_t mf_desfire_auth_cmd(MfDesfireKeyType t) {
    switch(t) {
    case MfDesfireKeyTypeAes: return 0xaa;
    case MfDesfireKeyType3k3Des: return 0x1a;
    default: return 0x0a; // DES / 2K3DES legacy
    }
}
void mf_desfire_rotate_left(uint8_t* d, size_t n) {
    if(n == 0) return;
    uint8_t first = d[0];
    memmove(d, d + 1, n - 1);
    d[n - 1] = first;
}

void mf_desfire_derive_session_key(
    MfDesfireKeyType t, const uint8_t* a, const uint8_t* b, uint8_t* sk) {
    switch(t) {
    case MfDesfireKeyTypeDes:
        memcpy(sk + 0, a, 4); memcpy(sk + 4, b, 4);
        break;
    case MfDesfireKeyType2k3Des:
        memcpy(sk + 0, a, 4);  memcpy(sk + 4, b, 4);
        memcpy(sk + 8, a + 4, 4); memcpy(sk + 12, b + 4, 4);
        break;
    case MfDesfireKeyType3k3Des:
        memcpy(sk + 0,  a, 4);      memcpy(sk + 4,  b, 4);
        memcpy(sk + 8,  a + 6, 4);  memcpy(sk + 12, b + 6, 4);
        memcpy(sk + 16, a + 12, 4); memcpy(sk + 20, b + 12, 4);
        break;
    case MfDesfireKeyTypeAes:
        memcpy(sk + 0, a, 4);       memcpy(sk + 4, b, 4);
        memcpy(sk + 8, a + 12, 4);  memcpy(sk + 12, b + 12, 4);
        break;
    }
}

// Expand DESFire key material into a 24-byte 3DES key for mbedtls_des3_set3key.
static void des_expand(MfDesfireKeyType t, const uint8_t* key, uint8_t out[24]) {
    if(t == MfDesfireKeyTypeDes) {           // K1=K2=K3
        memcpy(out, key, 8); memcpy(out + 8, key, 8); memcpy(out + 16, key, 8);
    } else if(t == MfDesfireKeyType2k3Des) { // K1,K2,K1
        memcpy(out, key, 16); memcpy(out + 16, key, 8);
    } else {                                  // 3K3DES: K1,K2,K3
        memcpy(out, key, 24);
    }
}

void mf_desfire_cbc_encrypt(
    MfDesfireKeyType t, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len) {
    if(t == MfDesfireKeyTypeAes) {
        mbedtls_aes_context c; mbedtls_aes_init(&c);
        mbedtls_aes_setkey_enc(&c, key, 128);
        mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
        mbedtls_aes_free(&c);
    } else {
        uint8_t k[24]; des_expand(t, key, k);
        mbedtls_des3_context c; mbedtls_des3_init(&c);
        mbedtls_des3_set3key_enc(&c, k);
        mbedtls_des3_crypt_cbc(&c, MBEDTLS_DES_ENCRYPT, len, iv, in, out);
        mbedtls_des3_free(&c);
    }
}
void mf_desfire_cbc_decrypt(
    MfDesfireKeyType t, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len) {
    if(t == MfDesfireKeyTypeAes) {
        mbedtls_aes_context c; mbedtls_aes_init(&c);
        mbedtls_aes_setkey_dec(&c, key, 128);
        mbedtls_aes_crypt_cbc(&c, MBEDTLS_AES_DECRYPT, len, iv, in, out);
        mbedtls_aes_free(&c);
    } else {
        uint8_t k[24]; des_expand(t, key, k);
        mbedtls_des3_context c; mbedtls_des3_init(&c);
        mbedtls_des3_set3key_dec(&c, k);
        mbedtls_des3_crypt_cbc(&c, MBEDTLS_DES_DECRYPT, len, iv, in, out);
        mbedtls_des3_free(&c);
    }
}

// AES/3K3DES: reader sends by encrypting. Legacy DES/2K3DES: reader sends by
// decrypting ("send by decrypting"), and receives by encrypting — the two
// helpers below encode that swap so auth_begin/auth_finish stay scheme-agnostic.
static void cbc_send(MfDesfireCryptoCtx* c, const uint8_t* key,
                     const uint8_t* in, uint8_t* out, size_t len) {
    if(c->type == MfDesfireKeyTypeAes || c->type == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(c->type, key, c->iv, in, out, len);
    else
        mf_desfire_cbc_decrypt(c->type, key, c->iv, in, out, len);
}
static void cbc_recv(MfDesfireCryptoCtx* c, const uint8_t* key,
                     const uint8_t* in, uint8_t* out, size_t len) {
    if(c->type == MfDesfireKeyTypeAes || c->type == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_decrypt(c->type, key, c->iv, in, out, len);
    else
        mf_desfire_cbc_encrypt(c->type, key, c->iv, in, out, len);
}

bool mf_desfire_auth_begin(
    MfDesfireCryptoCtx* ctx, MfDesfireKeyType type, const uint8_t* key,
    const uint8_t* enc_rndb, const uint8_t* rnda, uint8_t* out_token) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;
    ctx->block_size = mf_desfire_block_size(type); // TRUE cipher block (Task 5 CMAC reads this)
    size_t bs = mf_desfire_challenge_size(type);    // RndA/RndB/token sizing for THIS handshake

    uint8_t rndb[16];
    memset(ctx->iv, 0, 16);
    // cbc_recv() is passed ctx->iv directly, and mbedtls_{aes,des3}_crypt_cbc
    // update the iv buffer in place to the correct next-chaining state after
    // processing `len` bytes (last real cipher sub-block: 16B for AES, 8B for
    // the DES family regardless of the 8/16-byte challenge length) — so
    // ctx->iv is already correct after this call. Do NOT re-derive it by
    // copying `bs` bytes of ciphertext manually: for 3K3DES bs (16, the
    // challenge length) differs from the cipher's 8-byte chaining granularity,
    // and re-copying the wrong slice would desync the running IV.
    cbc_recv(ctx, key, enc_rndb, rndb, bs);   // RndB
    memcpy(ctx->rndb, rndb, bs);              // stashed for session-key derivation in auth_finish

    memcpy(ctx->rnda, rnda, bs);
    uint8_t rndb_rot[16]; memcpy(rndb_rot, rndb, bs); mf_desfire_rotate_left(rndb_rot, bs);

    uint8_t plain[32];
    memcpy(plain, rnda, bs);
    memcpy(plain + bs, rndb_rot, bs);
    cbc_send(ctx, key, plain, out_token, bs * 2); // ctx->iv auto-updated (see note above)
    return true;
}

bool mf_desfire_auth_finish(
    MfDesfireCryptoCtx* ctx, const uint8_t* key, const uint8_t* enc_rndap) {
    size_t bs = mf_desfire_challenge_size(ctx->type); // RndA''/rotate sizing, not ctx->block_size
    uint8_t rndap[16];
    cbc_recv(ctx, key, enc_rndap, rndap, bs); // ctx->iv auto-updated (see auth_begin note)

    uint8_t rnda_rot[16]; memcpy(rnda_rot, ctx->rnda, bs); mf_desfire_rotate_left(rnda_rot, bs);
    if(memcmp(rndap, rnda_rot, bs) != 0) return false;

    mf_desfire_derive_session_key(ctx->type, ctx->rnda, ctx->rndb, ctx->session_key);
    memset(ctx->iv, 0, 16);
    ctx->authenticated = true;
    return true;
}
