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
uint8_t mf_desfire_auth_cmd(MfDesfireKeyType type);
void    mf_desfire_rotate_left(uint8_t* data, size_t len);
void    mf_desfire_cbc_encrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);
void    mf_desfire_cbc_decrypt(
    MfDesfireKeyType type, const uint8_t* key, uint8_t* iv,
    const uint8_t* in, uint8_t* out, size_t len);
