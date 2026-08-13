#include "ducky_os_cond.h"

#ifndef PUBLIC_BUILD

#include <string.h>

/* --- small token helpers ------------------------------------------------ */

/* Skip leading spaces. */
static const char* skip_spaces(const char* s) {
    while(*s == ' ') s++;
    return s;
}

/* True if the token [tok, tok+toklen) is exactly the keyword `kw`. */
static bool token_eq(const char* tok, size_t toklen, const char* kw) {
    size_t kwlen = strlen(kw);
    return toklen == kwlen && strncmp(tok, kw, kwlen) == 0;
}

/* Parse an OS name token into a DuckyOs. Returns false if unrecognized. */
static bool parse_os(const char* tok, size_t toklen, DuckyOs* out) {
    if(token_eq(tok, toklen, "WINDOWS")) {
        *out = DUCKY_OS_WINDOWS;
        return true;
    }
    if(token_eq(tok, toklen, "MAC") || token_eq(tok, toklen, "MACOS")) {
        *out = DUCKY_OS_MAC;
        return true;
    }
    if(token_eq(tok, toklen, "LINUX")) {
        *out = DUCKY_OS_LINUX;
        return true;
    }
    return false;
}

/* --- public API ----------------------------------------------------------*/

void ducky_os_cond_reset(DuckyOsCondState* st, DuckyOs initial_os) {
    st->target_os = initial_os;
    st->depth = 0;
    memset(st->execute, 0, sizeof(st->execute));
    memset(st->matched, 0, sizeof(st->matched));
    memset(st->in_else, 0, sizeof(st->in_else));
}

bool ducky_os_cond_skipping(const DuckyOsCondState* st) {
    return st->depth > 0 && !st->execute[st->depth - 1];
}

DuckyCondResult ducky_os_cond_eval(DuckyOsCondState* st, const char* line) {
    const char* p = skip_spaces(line);
    size_t toklen = strcspn(p, " ");
    bool skipping = ducky_os_cond_skipping(st);

    if(token_eq(p, toklen, "IF_OS")) {
        if(st->depth == DUCKY_OS_COND_MAX_DEPTH) return DUCKY_COND_ERROR;

        const char* arg = skip_spaces(p + toklen);
        size_t arglen = strcspn(arg, " ");
        DuckyOs os;
        bool ok = parse_os(arg, arglen, &os);
        // FAIL-CLOSED on a malformed/unknown OS name: still push a frame, but one
        // that never matches, so the guarded body is SKIPPED (not run on every OS)
        // and its END_IF stays balanced. Return ERROR so the typo is still logged.
        bool parent_execute = (st->depth == 0) ? true : st->execute[st->depth - 1];
        st->matched[st->depth] = ok && (os == st->target_os);
        st->in_else[st->depth] = false;
        st->execute[st->depth] = parent_execute && st->matched[st->depth];
        st->depth++;
        return ok ? DUCKY_COND_DIRECTIVE : DUCKY_COND_ERROR;
    }

    if(token_eq(p, toklen, "ELSE_OS")) {
        if(st->depth == 0) return DUCKY_COND_ERROR;
        uint8_t top = st->depth - 1;
        st->in_else[top] = true;
        bool parent_execute = (top == 0) ? true : st->execute[top - 1];
        st->execute[top] = parent_execute && !st->matched[top];
        return DUCKY_COND_DIRECTIVE;
    }

    if(token_eq(p, toklen, "END_IF")) {
        if(st->depth == 0) return DUCKY_COND_ERROR;
        st->depth--;
        return DUCKY_COND_DIRECTIVE;
    }

    if(token_eq(p, toklen, "SET_OS")) {
        const char* arg = skip_spaces(p + toklen);
        size_t arglen = strcspn(arg, " ");
        DuckyOs os;
        if(!parse_os(arg, arglen, &os)) return DUCKY_COND_ERROR;
        if(!skipping) st->target_os = os;
        return DUCKY_COND_DIRECTIVE;
    }

    return skipping ? DUCKY_COND_SKIP : DUCKY_COND_EXECUTE;
}

#endif /* PUBLIC_BUILD */
