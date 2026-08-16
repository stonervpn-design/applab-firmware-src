// virtual_card.c  — mirrors the reader math to validate the handshake wiring
#include "virtual_card.h"
#include <string.h>
static MfDesfireKeyType s_t; static uint8_t s_key[24]; static uint8_t s_rndb[16]; static size_t s_bs;
static uint8_t s_iv[16];
void vcard_init(MfDesfireKeyType t, const uint8_t* key, const uint8_t* rndb) {
    s_t = t; s_bs = mf_desfire_block_size(t);
    memcpy(s_key, key, mf_desfire_key_size(t));
    memcpy(s_rndb, rndb, s_bs);
}
size_t vcard_block(void) { return s_bs; }
void vcard_challenge(uint8_t* out) {
    memset(s_iv, 0, 16);
    // s_iv is passed directly as the mutable iv arg below, and
    // mbedtls_{aes,des3}_crypt_cbc already update it in place to the correct
    // next-chaining IV (last real cipher sub-block) after the call — do not
    // re-derive it by copying `s_bs` bytes of ciphertext manually, since for
    // 3K3DES s_bs (16, the challenge length) differs from the cipher's 8-byte
    // chaining granularity and would desync the running IV (mirrors the fix
    // in mf_desfire_auth_begin/finish).
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, s_rndb, out, s_bs);
    else
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, s_rndb, out, s_bs);
}
bool vcard_respond(const uint8_t* token, uint8_t* enc_rndap_out) {
    uint8_t plain[32];
    // decrypt reader token (2 blocks) with running IV; s_iv auto-updated (see note above)
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, token, plain, s_bs * 2);
    else
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, token, plain, s_bs * 2);
    // plain = RndA || rot_left(RndB); verify second block == rot_left(RndB)
    uint8_t rb[16]; memcpy(rb, s_rndb, s_bs); mf_desfire_rotate_left(rb, s_bs);
    if(memcmp(plain + s_bs, rb, s_bs) != 0) return false;
    // respond with E(rot_left(RndA))
    uint8_t ra[16]; memcpy(ra, plain, s_bs); mf_desfire_rotate_left(ra, s_bs);
    if(s_t == MfDesfireKeyTypeAes || s_t == MfDesfireKeyType3k3Des)
        mf_desfire_cbc_encrypt(s_t, s_key, s_iv, ra, enc_rndap_out, s_bs);
    else
        mf_desfire_cbc_decrypt(s_t, s_key, s_iv, ra, enc_rndap_out, s_bs);
    return true;
}
