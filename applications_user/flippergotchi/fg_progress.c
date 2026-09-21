#include "fg_progress.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char* const FG_NAMES[] =
    {"Pixel", "Nibble", "Byte", "Sparky", "Gizmo", "Waffle", "Mochi", "Bolt"};
#define FG_NAMES_N (sizeof(FG_NAMES) / sizeof(FG_NAMES[0]))

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

uint32_t fg_xp_for_level(uint32_t level) {
    return 10u * level * (level + 1) / 2u;
}

uint32_t fg_level_of(uint32_t xp) {
    uint32_t L = 0;
    while(fg_xp_for_level(L + 1) <= xp) L++;
    return L;
}

void fg_progress_add_xp(FgProgress* p, uint32_t dxp) {
    p->xp += dxp;
}

void fg_progress_default_name(FgProgress* p, uint32_t seed) {
    const char* n = FG_NAMES[xorshift(seed ? seed : 1) % FG_NAMES_N];
    strncpy(p->name, n, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
}

uint32_t fg_award_check(
    FgProgress* p,
    uint32_t hs,
    uint32_t pmkid,
    uint32_t friends,
    uint32_t full_hs,
    uint32_t session_secs,
    uint8_t channels_hit) {
    uint32_t newly = 0;
    struct {
        uint32_t bit;
        bool cond;
    } tbl[] = {
        {FG_ACH_FIRST, (hs + pmkid) >= 1},
        {FG_ACH_HS10, hs >= 10},
        {FG_ACH_PMKID25, pmkid >= 25},
        {FG_ACH_FRIEND1, friends >= 1},
        {FG_ACH_FRIEND5, friends >= 5},
        {FG_ACH_FULL, full_hs >= 1},
        {FG_ACH_NIGHTOWL, session_secs >= 1800},
        {FG_ACH_SURFER, channels_hit >= 8},
    };
    for(size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if(tbl[i].cond && !(p->ach & tbl[i].bit)) {
            p->ach |= tbl[i].bit;
            newly |= tbl[i].bit;
        }
    }
    return newly;
}

const char* fg_ach_name(uint32_t bit) {
    switch(bit) {
    case FG_ACH_FIRST: return "First Blood";
    case FG_ACH_HS10: return "Handshake x10";
    case FG_ACH_PMKID25: return "PMKID Master";
    case FG_ACH_FRIEND1: return "Socialite";
    case FG_ACH_FRIEND5: return "Grid Friend x5";
    case FG_ACH_FULL: return "Full House";
    case FG_ACH_NIGHTOWL: return "Night Owl";
    case FG_ACH_SURFER: return "Channel Surfer";
    default: return "?";
    }
}

// state file: "v=2;name=..;pwn=..;xp=..;ach=<hex>;bestses=..;beststreak=.."
void fg_progress_save(Storage* s, const FgProgress* p) {
    storage_common_mkdir(s, "/ext/apps_data");
    File* f = storage_file_alloc(s);
    if(storage_file_open(f, FG_STATE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[160];
        int n = snprintf(
            buf, sizeof(buf), "v=2;name=%s;pwn=%lu;xp=%lu;ach=%lx;bestses=%lu;beststreak=%lu\n",
            p->name, (unsigned long)p->total_pwns, (unsigned long)p->xp, (unsigned long)p->ach,
            (unsigned long)p->best_session, (unsigned long)p->best_streak);
        storage_file_write(f, buf, n);
        storage_file_close(f);
    }
    storage_file_free(f);
}

static void kv_apply(FgProgress* p, const char* key, const char* val) {
    if(strcmp(key, "name") == 0) {
        strncpy(p->name, val, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
    } else if(strcmp(key, "pwn") == 0)
        p->total_pwns = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(key, "xp") == 0)
        p->xp = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(key, "ach") == 0)
        p->ach = (uint32_t)strtoul(val, NULL, 16);
    else if(strcmp(key, "bestses") == 0)
        p->best_session = (uint32_t)strtoul(val, NULL, 10);
    else if(strcmp(key, "beststreak") == 0)
        p->best_streak = (uint32_t)strtoul(val, NULL, 10);
}

bool fg_progress_load(Storage* s, FgProgress* p) {
    memset(p, 0, sizeof(*p));
    File* f = storage_file_alloc(s);
    bool found = false;
    if(storage_file_open(f, FG_STATE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[192] = {0};
        uint16_t rd = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[rd] = '\0';
        storage_file_close(f);
        // split on ';' then 'key=val'
        char* save = NULL;
        for(char* tok = strtok_r(buf, ";\n", &save); tok; tok = strtok_r(NULL, ";\n", &save)) {
            char* eq = strchr(tok, '=');
            if(!eq) continue;
            *eq = '\0';
            kv_apply(p, tok, eq + 1);
        }
        found = p->name[0] != '\0';
    }
    storage_file_free(f);
    if(!found) {
        // Migrate the legacy bare-integer .score file, if present.
        File* g = storage_file_alloc(s);
        if(storage_file_open(g, FG_LEGACY_SCORE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            char b[16] = {0};
            uint16_t rd = storage_file_read(g, b, sizeof(b) - 1);
            b[rd] = '\0';
            p->total_pwns = (uint32_t)strtoul(b, NULL, 10);
            storage_file_close(g);
        }
        storage_file_free(g);
    }
    return found;
}
