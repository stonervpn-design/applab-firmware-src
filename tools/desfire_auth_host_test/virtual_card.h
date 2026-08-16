// virtual_card.h
#pragma once
#include "mf_desfire_crypto.h"
void   vcard_init(MfDesfireKeyType type, const uint8_t* key, const uint8_t* card_rndb);
size_t vcard_block(void);
void   vcard_challenge(uint8_t* enc_rndb_out);      // returns E(RndB)
bool   vcard_respond(const uint8_t* token, uint8_t* enc_rndap_out); // verifies, returns E(RndA'')
