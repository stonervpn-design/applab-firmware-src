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
    return (t == MfDesfireKeyTypeAes) ? 16 : 8;
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
