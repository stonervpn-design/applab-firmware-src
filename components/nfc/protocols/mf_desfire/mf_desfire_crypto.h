// mf_desfire_crypto.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MF_DESFIRE_CRYPTO_KEY_MAX   24
#define MF_DESFIRE_CRYPTO_BLOCK_MAX 16

typedef enum {
    MfDesfireKeyTypeDes,     // 8-byte key, single DES (native DESFire DES)
    MfDesfireKeyType2k3Des,  // 16-byte key, 2-key 3DES
    MfDesfireKeyType3k3Des,  // 24-byte key, 3-key 3DES
    MfDesfireKeyTypeAes,     // 16-byte key, AES-128
} MfDesfireKeyType;

size_t  mf_desfire_key_size(MfDesfireKeyType type);
size_t  mf_desfire_block_size(MfDesfireKeyType type);
size_t  mf_desfire_challenge_size(MfDesfireKeyType type);
uint8_t mf_desfire_auth_cmd(MfDesfireKeyType type);
void    mf_desfire_rotate_left(uint8_t* data, size_t len);
void    mf_desfire_derive_session_key(
    MfDesfireKeyType type, const uint8_t* rnda, const uint8_t* rndb, uint8_t* out_sk);
void    mf_desfire_cbc_encrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);
void    mf_desfire_cbc_decrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);

typedef struct {
    MfDesfireKeyType type;
    uint8_t session_key[MF_DESFIRE_CRYPTO_KEY_MAX];
    size_t  block_size;
    uint8_t iv[MF_DESFIRE_CRYPTO_BLOCK_MAX];
    uint8_t rnda[MF_DESFIRE_CRYPTO_BLOCK_MAX];
    uint8_t rndb[MF_DESFIRE_CRYPTO_BLOCK_MAX];
    uint8_t cmac_sk1[MF_DESFIRE_CRYPTO_BLOCK_MAX]; // filled in Task 5
    uint8_t cmac_sk2[MF_DESFIRE_CRYPTO_BLOCK_MAX]; // filled in Task 5
    bool    authenticated;
} MfDesfireCryptoCtx;

bool mf_desfire_auth_begin(
    MfDesfireCryptoCtx* ctx, MfDesfireKeyType type, const uint8_t* key,
    const uint8_t* enc_rndb, const uint8_t* rnda, uint8_t* out_token);
bool mf_desfire_auth_finish(
    MfDesfireCryptoCtx* ctx, const uint8_t* key, const uint8_t* enc_rndap);

// CMAC (RFC 4493 subkey generation + CBC-MAC), used to integrity-check
// MAC-comm file reads after a successful authenticate. Keys off
// ctx->block_size (the TRUE cipher block: 8 for DES/2K3DES/3K3DES, 16 for
// AES) for the Rb constant, block iteration/padding, and subkey length.
void mf_desfire_cmac_init(MfDesfireCryptoCtx* ctx);
void mf_desfire_cmac(MfDesfireCryptoCtx* ctx, const uint8_t* data, size_t len, uint8_t* mac8);
