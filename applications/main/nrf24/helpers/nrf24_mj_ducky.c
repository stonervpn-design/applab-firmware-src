#include "nrf24_mj_ducky.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <toolbox/stream/stream.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "Nrf24MjDucky"

#define MJ_DUCKY_INTER_KEY_MS 10

/* Upper bound on REPEAT N so a bogus/huge count can't hang the device. */
#define MJ_DUCKY_MAX_REPEAT 1000

/* Count lines once at open time so the UI can show progress. */
static size_t count_lines(Storage* storage, const char* path) {
    Stream* s = buffered_file_stream_alloc(storage);
    if(!buffered_file_stream_open(s, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        buffered_file_stream_close(s);
        stream_free(s);
        return 0;
    }
    FuriString* line = furi_string_alloc();
    size_t count = 0;
    while(stream_read_line(s, line)) count++;
    furi_string_free(line);
    buffered_file_stream_close(s);
    stream_free(s);
    return count;
}

MjScript* mj_script_open(Storage* storage, const char* path) {
    MjScript* s = malloc(sizeof(MjScript));
    if(!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->storage = storage;

    s->total_lines = count_lines(storage, path);

    Stream* stream = buffered_file_stream_alloc(storage);
    if(!buffered_file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_W(TAG, "open failed: %s", path);
        buffered_file_stream_close(stream);
        stream_free(stream);
        free(s);
        return NULL;
    }
    s->stream = stream;
    s->line_buf = furi_string_alloc();
    s->last_line = furi_string_alloc();
    s->default_delay_ms = 0;

    /* Extract basename for display. */
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    strncpy(s->file_name, base, sizeof(s->file_name) - 1);
    s->file_name[sizeof(s->file_name) - 1] = '\0';
    return s;
}

void mj_script_close(MjScript* s) {
    if(!s) return;
    if(s->stream) {
        buffered_file_stream_close((Stream*)s->stream);
        stream_free((Stream*)s->stream);
    }
    if(s->line_buf) furi_string_free((FuriString*)s->line_buf);
    if(s->last_line) furi_string_free((FuriString*)s->last_line);
    free(s);
}

bool mj_script_done(const MjScript* s) {
    return s->eof || s->error;
}

/* Strip leading & trailing whitespace + \r in place. */
static void trim_inplace(FuriString* fs) {
    /* Trim trailing CR/LF/space. */
    while(furi_string_size(fs) > 0) {
        char c = furi_string_get_char(fs, furi_string_size(fs) - 1);
        if(c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            furi_string_left(fs, furi_string_size(fs) - 1);
        } else {
            break;
        }
    }
    /* Trim leading. */
    size_t start = 0;
    while(start < furi_string_size(fs)) {
        char c = furi_string_get_char(fs, start);
        if(c == ' ' || c == '\t') start++;
        else break;
    }
    if(start > 0) furi_string_right(fs, start);
}

/* Yield in 50-ms chunks while polling abort. */
static void wait_with_abort(uint32_t ms, volatile bool* abort) {
    while(ms > 0) {
        if(abort && *abort) return;
        uint32_t chunk = ms > 50 ? 50 : ms;
        furi_delay_ms(chunk);
        ms -= chunk;
    }
}

/* Match `prefix` (case-insensitive) followed by space/tab; on hit return ptr
 * to argument start (skipping further whitespace). Returns NULL on miss. */
static const char* match_directive(const char* line, const char* prefix) {
    size_t pl = strlen(prefix);
    if(strncasecmp(line, prefix, pl) != 0) return NULL;
    char c = line[pl];
    if(c != ' ' && c != '\t' && c != '\0') return NULL;
    while(line[pl] == ' ' || line[pl] == '\t') pl++;
    return line + pl;
}

static bool starts_with_ci(const char* line, const char* prefix) {
    size_t pl = strlen(prefix);
    return strncasecmp(line, prefix, pl) == 0;
}

/* Parse a key/modifier combo line (no STRING / DELAY / etc. prefix).
 * Tokens are space-separated; modifiers accumulate, last keycode wins. */
static bool parse_combo(const char* line, uint8_t* mod_out, uint8_t* key_out) {
    uint8_t mod = 0;
    uint8_t key = MJ_KEY_NONE;
    char tok[24];

    while(*line) {
        while(*line == ' ' || *line == '\t') line++;
        if(!*line) break;
        size_t i = 0;
        while(*line && *line != ' ' && *line != '\t' && i < sizeof(tok) - 1) {
            tok[i++] = *line++;
        }
        tok[i] = '\0';

        if(i == 1) {
            MjHidKey hk;
            if(mj_ascii_to_hid(tok[0], &hk)) {
                mod |= hk.modifier;
                key = hk.keycode;
                continue;
            }
        }

        uint8_t m, k;
        if(mj_lookup_ducky_key(tok, &m, &k)) {
            mod |= m;
            if(k != MJ_KEY_NONE) key = k;
            continue;
        }
        return false;
    }

    *mod_out = mod;
    *key_out = key;
    return true;
}

/* Execute one already-trimmed, non-empty, non-comment, non-REPEAT line.
 * Shared by the normal step path and by REPEAT (which re-feeds the buffered
 * previous line). Never handles REPEAT itself, so it cannot recurse. */
static void process_line(MjScript* s, const char* line, const MjTarget* target, volatile bool* abort) {
    /* DEFAULTDELAY / DEFAULT_DELAY */
    const char* arg;
    if((arg = match_directive(line, "DEFAULT_DELAY")) ||
       (arg = match_directive(line, "DEFAULTDELAY"))) {
        int v = atoi(arg);
        if(v < 0) v = 0;
        if(v > 10000) v = 10000;
        s->default_delay_ms = (uint16_t)v;
        return;
    }

    /* DELAY */
    if((arg = match_directive(line, "DELAY"))) {
        int v = atoi(arg);
        if(v < 1) v = 1;
        if(v > 60000) v = 60000;
        wait_with_abort((uint32_t)v, abort);
        return;
    }

    /* STRINGLN */
    if((arg = match_directive(line, "STRINGLN"))) {
        mj_inject_string(target, arg, abort);
        if(!(abort && *abort)) {
            mj_inject_keystroke(target, MJ_MOD_NONE, MJ_KEY_ENTER);
        }
        if(s->default_delay_ms) wait_with_abort(s->default_delay_ms, abort);
        return;
    }

    /* STRING */
    if((arg = match_directive(line, "STRING"))) {
        mj_inject_string(target, arg, abort);
        if(s->default_delay_ms) wait_with_abort(s->default_delay_ms, abort);
        return;
    }

    /* Combo / single key / chord */
    uint8_t mod, key;
    if(parse_combo(line, &mod, &key)) {
        if(mod || key) {
            mj_inject_keystroke(target, mod, key);
            furi_delay_ms(MJ_DUCKY_INTER_KEY_MS);
        }
        if(s->default_delay_ms) wait_with_abort(s->default_delay_ms, abort);
        return;
    }

    snprintf(
        s->last_warning,
        sizeof(s->last_warning),
        "L%u: unknown '%.20s'",
        (unsigned)s->current_line,
        line);
}

bool mj_script_step(MjScript* s, const MjTarget* target, volatile bool* abort) {
    if(!s || s->eof || s->error) return false;
    if(abort && *abort) return false;

    Stream* stream = (Stream*)s->stream;
    FuriString* fs = (FuriString*)s->line_buf;

    if(!stream_read_line(stream, fs)) {
        s->eof = true;
        return false;
    }
    s->current_line++;
    trim_inplace(fs);

    if(furi_string_size(fs) == 0) return true;

    const char* line = furi_string_get_cstr(fs);

    /* Comments */
    if(starts_with_ci(line, "REM") || line[0] == '/' || line[0] == '#') {
        return true;
    }

    /* REPEAT N — BadUSB-style "re-run the previous executable line N times".
     * REPEAT is never itself buffered, so it cannot repeat itself; the source
     * line reference (s->last_line) is a distinct buffer from line_buf, so it
     * stays stable while we re-feed it. */
    const char* arg;
    if((arg = match_directive(line, "REPEAT"))) {
        FuriString* prev = (FuriString*)s->last_line;
        if(furi_string_size(prev) == 0) {
            snprintf(
                s->last_warning,
                sizeof(s->last_warning),
                "L%u: REPEAT with no prior line",
                (unsigned)s->current_line);
            return true;
        }
        int n = atoi(arg);
        if(n < 1) n = 1;
        if(n > MJ_DUCKY_MAX_REPEAT) n = MJ_DUCKY_MAX_REPEAT;
        const char* prev_cstr = furi_string_get_cstr(prev);
        for(int i = 0; i < n; i++) {
            if(abort && *abort) break;
            process_line(s, prev_cstr, target, abort);
        }
        return true;
    }

    /* Executable line: remember it so a following REPEAT can replay it, then
     * run it. */
    furi_string_set((FuriString*)s->last_line, fs);
    process_line(s, line, target, abort);
    return true;
}
