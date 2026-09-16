#include "subghz_proto_filter.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>

#define TAG "SubGhzProtoFilter"

#define SUBGHZ_PROTO_FILTER_DIR  EXT_PATH("subghz/assets")
#define SUBGHZ_PROTO_FILTER_PATH EXT_PATH("subghz/assets/proto_filter.txt")

// In-memory OFF set: protocol names joined by '\n', kept with a leading AND
// trailing '\n' so an exact membership test is a search for "\n<name>\n". This
// avoids substring false-hits (e.g. "Kia" inside "KiaRIO"). NULL until first load.
static FuriString* s_off = NULL;
static bool s_loaded = false;
static bool s_batch = false;

static void subghz_proto_filter_ensure_loaded(void) {
    if(s_loaded) return;
    if(s_off == NULL) s_off = furi_string_alloc();
    furi_string_set(s_off, "\n"); // empty set == a single sentinel newline

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, SUBGHZ_PROTO_FILTER_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(stream, line)) {
            furi_string_trim(line); // strip CR/LF and surrounding blanks
            if(furi_string_size(line) == 0) continue;
            furi_string_cat(s_off, line); // s_off already ends with '\n'
            furi_string_push_back(s_off, '\n');
        }
        furi_string_free(line);
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    s_loaded = true;
}

static void subghz_proto_filter_save(void) {
    subghz_proto_filter_ensure_loaded();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("subghz"));
    storage_simply_mkdir(storage, SUBGHZ_PROTO_FILTER_DIR);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, SUBGHZ_PROTO_FILTER_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        // s_off is "\nName1\nName2\n" (or just "\n" when empty). Writing it verbatim
        // yields one leading blank line, which the loader skips — harmless.
        stream_write_string(stream, s_off);
    } else {
        FURI_LOG_E(TAG, "write failed: %s", SUBGHZ_PROTO_FILTER_PATH);
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

bool subghz_proto_filter_contains(const char* name) {
    if(name == NULL || name[0] == '\0') return false;
    subghz_proto_filter_ensure_loaded();
    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\n%s\n", name);
    if(n <= 0 || n >= (int)sizeof(needle)) return false; // name too long to have been stored
    return furi_string_search_str(s_off, needle, 0) != FURI_STRING_FAILURE;
}

bool subghz_proto_filter_set(const char* name, bool disabled) {
    if(name == NULL || name[0] == '\0') return false;
    subghz_proto_filter_ensure_loaded();

    bool present = subghz_proto_filter_contains(name);
    if(disabled == present) return false; // already in the desired state

    if(disabled) {
        furi_string_cat_str(s_off, name); // s_off ends with '\n'
        furi_string_push_back(s_off, '\n');
    } else {
        FuriString* needle = furi_string_alloc_set("\n");
        furi_string_cat_str(needle, name);
        furi_string_push_back(needle, '\n');
        size_t pos = furi_string_search_str(s_off, furi_string_get_cstr(needle), 0);
        if(pos != FURI_STRING_FAILURE) {
            // Erase "<name>\n" (needle without its leading '\n'), keeping the
            // leading '\n' so the sentinel structure stays intact.
            furi_string_replace_at(s_off, pos + 1, furi_string_size(needle) - 1, "");
        }
        furi_string_free(needle);
    }

    if(!s_batch) subghz_proto_filter_save();
    return true;
}

void subghz_proto_filter_clear_all(void) {
    subghz_proto_filter_ensure_loaded();
    furi_string_set(s_off, "\n");
    if(!s_batch) subghz_proto_filter_save();
}

void subghz_proto_filter_begin_batch(void) {
    subghz_proto_filter_ensure_loaded();
    s_batch = true;
}

void subghz_proto_filter_end_batch(void) {
    s_batch = false;
    subghz_proto_filter_save();
}

void subghz_proto_filter_reload(void) {
    s_loaded = false;
    subghz_proto_filter_ensure_loaded();
}
