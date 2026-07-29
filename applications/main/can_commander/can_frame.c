#include "can_frame.h"
#include <string.h>

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_sep(char c) {
    return c == ' ' || c == ',' || c == ':' || c == '\t';
}

bool can_frame_parse_id(const char* s, bool force_ext, uint32_t* out_id, bool* out_ext) {
    if(!s || !out_id || !out_ext) return false;
    while(is_sep(*s)) s++;
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if(*s == '\0') return false;

    uint32_t id = 0;
    size_t digits = 0;
    for(; *s; s++) {
        if(is_sep(*s)) break;
        int n = hex_nibble(*s);
        if(n < 0) return false;
        if(digits >= 8) return false; // more than 32 bits of hex
        id = (id << 4) | (uint32_t)n;
        digits++;
    }
    if(digits == 0) return false;
    if(id > CAN_ID_EXT_MAX) return false;

    bool ext = force_ext || (id > CAN_ID_STD_MAX);
    if(!ext && id > CAN_ID_STD_MAX) return false;
    *out_id = id;
    *out_ext = ext;
    return true;
}

bool can_frame_parse_data(const char* s, uint8_t* out, uint8_t* out_len) {
    if(!out || !out_len) return false;
    *out_len = 0;
    if(!s) return true; // empty = zero bytes

    uint8_t count = 0;
    while(*s) {
        while(is_sep(*s)) s++;
        if(*s == '\0') break;
        int hi = hex_nibble(*s++);
        if(hi < 0) return false;
        if(*s == '\0' || is_sep(*s)) return false; // odd nibble
        int lo = hex_nibble(*s++);
        if(lo < 0) return false;
        if(count >= CAN_MAX_DATA) return false;
        out[count++] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = count;
    return true;
}

bool can_frame_build(
    const char* id_str,
    const char* data_str,
    bool force_ext,
    bool rtr,
    CanFrame* out) {
    if(!out) return false;
    CanFrame f;
    memset(&f, 0, sizeof(f));
    if(!can_frame_parse_id(id_str, force_ext, &f.id, &f.extended)) return false;
    f.rtr = rtr;
    if(rtr) {
        f.dlc = 0;
    } else {
        if(!can_frame_parse_data(data_str, f.data, &f.dlc)) return false;
    }
    *out = f;
    return true;
}

static size_t append_hex_byte(char* out, size_t pos, size_t out_max, uint8_t b) {
    static const char* H = "0123456789ABCDEF";
    if(pos + 2 >= out_max) return pos;
    out[pos++] = H[(b >> 4) & 0xF];
    out[pos++] = H[b & 0xF];
    return pos;
}

size_t can_frame_format(const CanFrame* f, char* out, size_t out_max) {
    if(!f || !out || out_max < 4) return 0;
    static const char* H = "0123456789ABCDEF";
    size_t pos = 0;

    // Identifier: min width 3 for std, 8 for ext.
    char idbuf[9];
    size_t idn = 0;
    uint32_t id = f->id;
    do {
        idbuf[idn++] = H[id & 0xF];
        id >>= 4;
    } while(id && idn < 8);
    size_t minw = f->extended ? 8 : 3;
    while(idn < minw && idn < 8) idbuf[idn++] = '0';
    while(idn > 0) {
        if(pos + 1 >= out_max) {
            out[pos] = '\0';
            return pos;
        }
        out[pos++] = idbuf[--idn];
    }

    if(pos + 1 >= out_max) {
        out[pos] = '\0';
        return pos;
    }
    out[pos++] = '#';

    if(f->rtr) {
        if(pos + 1 < out_max) out[pos++] = 'R';
    } else {
        for(uint8_t i = 0; i < f->dlc; i++) {
            pos = append_hex_byte(out, pos, out_max, f->data[i]);
        }
    }
    out[pos < out_max ? pos : out_max - 1] = '\0';
    return pos;
}

size_t can_frame_preset_encode(const char* name, const CanFrame* f, char* out, size_t out_max) {
    if(!name || !f || !out) return 0;
    // "name,ext,rtr,idhex,datahex"
    char idbuf[16];
    CanFrame idonly = *f;
    idonly.rtr = false;
    idonly.dlc = 0;
    // reuse formatter for the id, then strip the trailing '#'
    size_t idn = can_frame_format(&idonly, idbuf, sizeof(idbuf));
    if(idn > 0 && idbuf[idn - 1] == '#') idbuf[idn - 1] = '\0';

    char databuf[24];
    size_t dp = 0;
    for(uint8_t i = 0; i < f->dlc; i++) dp = append_hex_byte(databuf, dp, sizeof(databuf), f->data[i]);
    databuf[dp] = '\0';

    // Manual, bounded compose (no snprintf dep in pure module).
    size_t pos = 0;
    const char* fields[5];
    char extc[2] = {f->extended ? '1' : '0', '\0'};
    char rtrc[2] = {f->rtr ? '1' : '0', '\0'};
    fields[0] = name;
    fields[1] = extc;
    fields[2] = rtrc;
    fields[3] = idbuf;
    fields[4] = databuf;
    for(int fi = 0; fi < 5; fi++) {
        const char* p = fields[fi];
        while(*p) {
            if(pos + 1 >= out_max) return 0;
            out[pos++] = *p++;
        }
        if(fi < 4) {
            if(pos + 1 >= out_max) return 0;
            out[pos++] = ',';
        }
    }
    out[pos] = '\0';
    return pos;
}

bool can_frame_preset_decode(const char* line, char* name_out, size_t name_max, CanFrame* out) {
    if(!line || !name_out || !out || name_max == 0) return false;
    // Split into 5 comma fields.
    const char* p = line;
    const char* fields[5];
    size_t lens[5];
    for(int i = 0; i < 5; i++) {
        fields[i] = p;
        const char* c = p;
        while(*c && *c != ',' && *c != '\r' && *c != '\n') c++;
        lens[i] = (size_t)(c - p);
        if(i < 4) {
            if(*c != ',') return false; // not enough fields
            p = c + 1;
        }
    }
    if(lens[0] >= name_max) return false;
    memcpy(name_out, fields[0], lens[0]);
    name_out[lens[0]] = '\0';

    bool ext = lens[1] > 0 && fields[1][0] == '1';
    bool rtr = lens[2] > 0 && fields[2][0] == '1';

    char idtmp[16], datatmp[24];
    if(lens[3] >= sizeof(idtmp) || lens[4] >= sizeof(datatmp)) return false;
    memcpy(idtmp, fields[3], lens[3]);
    idtmp[lens[3]] = '\0';
    memcpy(datatmp, fields[4], lens[4]);
    datatmp[lens[4]] = '\0';

    return can_frame_build(idtmp, rtr ? "" : datatmp, ext, rtr, out);
}
