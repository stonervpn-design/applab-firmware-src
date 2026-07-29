#include "whisperpair_fp.h"

#include <stddef.h>
#include <string.h>

#define FP_SERVICE_UUID16 0xFE2C

FpAdvKind fp_adv_parse(const uint8_t* buf, uint8_t len, uint32_t* model_id_out) {
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;              // zero-length field: end of data
        if(pos + fld_len >= len) break;      // field overruns buffer: stop, no over-read
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;

        // AD type 0x16 = Service Data - 16-bit UUID: [uuid_lo, uuid_hi, data...]
        if(type == 0x16 && val_len >= 2) {
            uint16_t uuid = (uint16_t)(val[0] | (val[1] << 8));
            if(uuid == FP_SERVICE_UUID16) {
                uint8_t data_len = (uint8_t)(val_len - 2);
                const uint8_t* data = &val[2];
                if(data_len == 3) {
                    if(model_id_out) {
                        *model_id_out = ((uint32_t)data[0] << 16) |
                                        ((uint32_t)data[1] << 8) |
                                        (uint32_t)data[2];
                    }
                    return FpAdvDiscoverable;
                }
                return FpAdvHasAccounts;
            }
        }

        pos += fld_len + 1;
    }
    return FpAdvNone;
}

// Known-vulnerable device names (substring, whisperpair.eu/vulnerable-devices).
// Match is a HINT layered on top of the surface verdict, never the sole signal.
static const char* const FP_VULN_NAMES[] = {
    "WH-1000XM6", "WH-1000XM5", "WH-1000XM4", "WF-1000XM5", "WH-CH720N",
    "Pixel Buds Pro 2", "Nord Buds 3 Pro", "Redmi Buds 5 Pro",
    "MOTIF II", "Liberty 4 NC", "Elite 8 Active", "Tune Beam", "Nothing Ear",
};

static char up(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static bool ci_contains(const char* hay, const char* needle) {
    if(!hay || !needle || !needle[0]) return false;
    for(const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while(*a && *b && up(*a) == up(*b)) {
            a++;
            b++;
        }
        if(!*b) return true;
    }
    return false;
}

bool fp_name_is_vuln(const char* name) {
    if(!name || !name[0]) return false;
    for(size_t i = 0; i < sizeof(FP_VULN_NAMES) / sizeof(FP_VULN_NAMES[0]); i++) {
        if(ci_contains(name, FP_VULN_NAMES[i])) return true;
    }
    return false;
}
