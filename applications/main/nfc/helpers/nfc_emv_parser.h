#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <storage/storage.h>

/** Get EMV application name by number
 * @param storage Storage instance
 * @param aid - AID number array
 * @param aid_len - AID length
 * @param aid_name - string to keep AID name
 * @return - true if AID found, false otherwise
 */
bool nfc_emv_parser_get_aid_name(
    Storage* storage,
    const uint8_t* aid,
    uint8_t aid_len,
    FuriString* aid_name);

/** Get country name by country code
 * @param storage Storage instance
 * @param country_code - ISO 3166 country code
 * @param country_name - string to keep country name
 * @return - true if country found, false otherwise
 */
bool nfc_emv_parser_get_country_name(
    Storage* storage,
    uint16_t country_code,
    FuriString* country_name);

/** Get currency name by currency code
 * @param storage Storage instance
 * @param currency_code - ISO 3166 currency code
 * @param currency_name - string to keep currency name
 * @return - true if currency found, false otherwise
 */
bool nfc_emv_parser_get_currency_name(
    Storage* storage,
    uint16_t currency_code,
    FuriString* currency_name);

/** Get issuing bank name by PAN BIN prefix (nfc/assets/bin.nfc).
 * The PAN's leading digits (BIN/IIN) identify the issuer. Tries the leading
 * 8..4 digits, longest first, so a specific 8-digit entry wins over a broad
 * 4-digit one.
 * @param storage Storage instance
 * @param pan - PAN bytes (BCD-packed, from EmvApplication.pan)
 * @param pan_len - PAN length in bytes
 * @param bank_name - string to keep the bank name
 * @return - true if a bank was found, false otherwise
 */
bool nfc_emv_parser_get_bank_name(
    Storage* storage,
    const uint8_t* pan,
    uint8_t pan_len,
    FuriString* bank_name);
