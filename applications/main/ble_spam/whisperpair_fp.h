#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fast Pair advertisement classification (Google Fast Pair spec):
//   0xFE2C service data, 3-byte payload  -> Model ID present; provider is
//                                           discoverable / in pairing mode.
//   0xFE2C service data, other length    -> Account-Key filter (Bloom); provider
//                                           is non-discoverable, has bonded accounts.
//   no 0xFE2C service data               -> not a Fast Pair advertisement.
typedef enum {
    FpAdvNone = 0,
    FpAdvDiscoverable,
    FpAdvHasAccounts,
} FpAdvKind;

// Walk a BLE advertisement TLV buffer for Service Data - 16-bit UUID (AD type
// 0x16) whose UUID is 0xFE2C. Returns the classification; when the payload is
// exactly the 3-byte Model ID, writes the 24-bit model id (big-endian in the
// advert, i.e. octet 0 is MSB) into *model_id_out (if non-NULL).
// Pure: no esp/furi deps; bounds-checked (no over-read on malformed input).
FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out);

// Case-insensitive substring match of `name` against the known-vulnerable
// device-name table (whisperpair.eu/vulnerable-devices). NULL/empty -> false.
bool fp_name_is_vuln(const char* name);
