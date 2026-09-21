#include "wlan_loot_manifest.h"

// Copy up to the next ',' (or end) from *pp into dst; advance *pp past the comma.
// Returns false if there was no field (NULL cursor).
static bool mf_field(const char** pp, char* dst, size_t dst_sz, bool last) {
    const char* p = *pp;
    if(!p) return false;
    size_t j = 0;
    while(*p && (last || *p != ',') && *p != '\n' && *p != '\r') {
        if(j < dst_sz - 1) dst[j++] = *p;
        p++;
    }
    dst[j] = '\0';
    if(!last) {
        if(*p == ',') { *pp = p + 1; }
        else { *pp = NULL; } // no more commas — field list ended early
    }
    return true;
}

bool wlan_manifest_parse_line(const char* line, WlanManifestRow* out) {
    if(!line || line[0] == '\0' || line[0] == '\n' || line[0] == '\r') return false;
    if(strncmp(line, "timestamp,", 10) == 0) return false; // header
    const char* p = line;
    if(!mf_field(&p, out->timestamp, sizeof(out->timestamp), false)) return false;
    if(!p || !mf_field(&p, out->type, sizeof(out->type), false)) return false;
    if(!p || !mf_field(&p, out->target, sizeof(out->target), false)) return false;
    if(!p) return false; // needed a 4th field
    mf_field(&p, out->detail, sizeof(out->detail), true);
    return true;
}
