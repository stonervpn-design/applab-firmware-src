#include "nfc_emv_parser.h"
#include <flipper_format/flipper_format.h>

static const char* nfc_resources_header = "Flipper EMV resources";
static const uint32_t nfc_resources_file_version = 1;

static bool nfc_emv_parser_search_data(
    Storage* storage,
    const char* file_name,
    FuriString* key,
    FuriString* data) {
    bool parsed = false;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* temp_str;
    temp_str = furi_string_alloc();

    do {
        // Open file
        if(!flipper_format_file_open_existing(file, file_name)) break;
        // Read file header and version
        uint32_t version = 0;
        if(!flipper_format_read_header(file, temp_str, &version)) break;
        if(furi_string_cmp_str(temp_str, nfc_resources_header) ||
           (version != nfc_resources_file_version))
            break;
        if(!flipper_format_read_string(file, furi_string_get_cstr(key), data)) break;
        parsed = true;
    } while(false);

    furi_string_free(temp_str);
    flipper_format_free(file);
    return parsed;
}

bool nfc_emv_parser_get_aid_name(
    Storage* storage,
    const uint8_t* aid,
    uint8_t aid_len,
    FuriString* aid_name) {
    furi_assert(storage);
    bool parsed = false;
    FuriString* key;
    key = furi_string_alloc();
    for(uint8_t i = 0; i < aid_len; i++) {
        furi_string_cat_printf(key, "%02X", aid[i]);
    }
    if(nfc_emv_parser_search_data(storage, EXT_PATH("nfc/assets/aid.nfc"), key, aid_name)) {
        parsed = true;
    }
    furi_string_free(key);
    return parsed;
}

bool nfc_emv_parser_get_country_name(
    Storage* storage,
    uint16_t country_code,
    FuriString* country_name) {
    bool parsed = false;
    FuriString* key;
    key = furi_string_alloc_printf("%04X", country_code);
    if(nfc_emv_parser_search_data(
           storage, EXT_PATH("nfc/assets/country_code.nfc"), key, country_name)) {
        parsed = true;
    }
    furi_string_free(key);
    return parsed;
}

bool nfc_emv_parser_get_currency_name(
    Storage* storage,
    uint16_t currency_code,
    FuriString* currency_name) {
    bool parsed = false;
    FuriString* key;
    key = furi_string_alloc_printf("%04X", currency_code);
    if(nfc_emv_parser_search_data(
           storage, EXT_PATH("nfc/assets/currency_code.nfc"), key, currency_name)) {
        parsed = true;
    }
    furi_string_free(key);
    return parsed;
}

bool nfc_emv_parser_get_bank_name(
    Storage* storage,
    const uint8_t* pan,
    uint8_t pan_len,
    FuriString* bank_name) {
    furi_assert(storage);
    if(!pan || pan_len == 0) return false;

    /* Build the decimal PAN digit string from the BCD-packed bytes (each byte
     * is two decimal digits; trailing 'F' nibbles are padding). */
    FuriString* digits = furi_string_alloc();
    const uint8_t clamped = pan_len > 10 ? 10 : pan_len;
    for(uint8_t i = 0; i < clamped; i++) {
        furi_string_cat_printf(digits, "%02X", pan[i]);
    }
    furi_string_trim(digits, "F");

    bool parsed = false;
    FuriString* key = furi_string_alloc();
    /* Longest-prefix-first: an 8-digit BIN entry (a specific card product) wins
     * over a broad 4-digit issuer range. */
    for(size_t len = 8; len >= 4; len--) {
        if(furi_string_size(digits) < len) continue;
        furi_string_set_n(key, digits, 0, len);
        if(nfc_emv_parser_search_data(storage, EXT_PATH("nfc/assets/bin.nfc"), key, bank_name)) {
            parsed = true;
            break;
        }
    }
    furi_string_free(key);
    furi_string_free(digits);
    return parsed;
}
