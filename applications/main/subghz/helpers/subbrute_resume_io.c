// FlipperFormat storage glue for resumable-brute records. Mirrors the persistence
// template in subbrute_settings.c (file_alloc -> open_always/existing ->
// header -> insert_or_update/read -> close/free), storage-gated. Kept separate
// from the pure validator (subbrute_resume.c) so that stays host-testable.
#include "subbrute_resume.h"

#ifndef PUBLIC_BUILD

#include <furi.h>
#include <furi_hal.h>
#include <flipper_format.h>
#include "subbrute_protocols.h" // SubBruteAttacks enum — for the drift asserts below

#define TAG "SubBruteResume"

#define SUBBRUTE_RESUME_FILE_TYPE    "Sub-GHz BruteForcer Resume File"
#define SUBBRUTE_RESUME_FILE_VERSION 1
#define SUBBRUTE_RESUME_PATH         APP_DATA_PATH("bruteforcer.resume")

#define F_SCHEMA   "Schema"
#define F_ATTACK   "Attack"
#define F_STEP     "CurrentStep"
#define F_REPEATS  "ExtraRepeats"
#define F_TWOBYTES "TwoBytes"
#define F_OPENCODE "Opencode"

// The header duplicates three SubBruteAttacks indices so the validator can stay
// furi-free; assert here (where the real enum is in scope) that they still match,
// so a reorder trips the build instead of silently mis-targeting an attack.
_Static_assert(
    SUBBRUTE_RESUME_ATTACK_TOTAL == SubBruteAttackTotalCount,
    "resume enum drift: total count changed");
_Static_assert(
    SUBBRUTE_RESUME_ATTACK_DEBRUIJN == SubBruteAttackDeBruijn,
    "resume enum drift: DeBruijn index changed");
_Static_assert(
    SUBBRUTE_RESUME_ATTACK_LOADFILE == SubBruteAttackLoadFile,
    "resume enum drift: LoadFile index changed");

bool subbrute_resume_save(const SubBruteResume* r) {
    if(!r) return false;

    bool saved = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);

    do {
        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_always(file, SUBBRUTE_RESUME_PATH)) break;
        if(!flipper_format_write_header_cstr(
               file, SUBBRUTE_RESUME_FILE_TYPE, SUBBRUTE_RESUME_FILE_VERSION))
            break;

        uint32_t schema = r->schema_version;
        uint32_t attack = r->attack;
        uint32_t repeats = r->extra_repeats;
        uint32_t two = r->two_bytes;
        uint32_t opencode = r->opencode;
        uint64_t step = r->current_step; // raw bytes; same MCU saves+loads

        if(!flipper_format_insert_or_update_uint32(file, F_SCHEMA, &schema, 1)) break;
        if(!flipper_format_insert_or_update_uint32(file, F_ATTACK, &attack, 1)) break;
        if(!flipper_format_insert_or_update_hex(file, F_STEP, (uint8_t*)&step, sizeof(step)))
            break;
        if(!flipper_format_insert_or_update_uint32(file, F_REPEATS, &repeats, 1)) break;
        if(!flipper_format_insert_or_update_uint32(file, F_TWOBYTES, &two, 1)) break;
        if(!flipper_format_insert_or_update_uint32(file, F_OPENCODE, &opencode, 1)) break;

        saved = true;
    } while(false);

    if(!saved) FURI_LOG_E(TAG, "resume save failed (%s)", SUBBRUTE_RESUME_PATH);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    return saved;
}

bool subbrute_resume_load(SubBruteResume* out) {
    if(!out) return false;

    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    SubBruteResume r = {0};

    do {
        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_existing(file, SUBBRUTE_RESUME_PATH)) break;

        uint32_t schema = 0, attack = 0, repeats = 0, two = 0, opencode = 0;
        if(!flipper_format_read_uint32(file, F_SCHEMA, &schema, 1)) break;
        if(!flipper_format_read_uint32(file, F_ATTACK, &attack, 1)) break;
        if(!flipper_format_read_hex(file, F_STEP, (uint8_t*)&r.current_step, sizeof(r.current_step)))
            break;
        if(!flipper_format_read_uint32(file, F_REPEATS, &repeats, 1)) break;
        if(!flipper_format_read_uint32(file, F_TWOBYTES, &two, 1)) break;
        if(!flipper_format_read_uint32(file, F_OPENCODE, &opencode, 1)) break;

        r.schema_version = schema;
        r.attack = attack;
        r.extra_repeats = (uint8_t)repeats;
        r.two_bytes = (uint8_t)two;
        r.opencode = (uint8_t)opencode;

        ok = subbrute_resume_is_valid(&r); // reject stale/DeBruijn/LoadFile/out-of-range
    } while(false);

    flipper_format_file_close(file);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    if(ok) *out = r;
    return ok;
}

bool subbrute_resume_has(void) {
    SubBruteResume r;
    return subbrute_resume_load(&r);
}

void subbrute_resume_clear(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, SUBBRUTE_RESUME_PATH);
    furi_record_close(RECORD_STORAGE);
}

#endif // PUBLIC_BUILD
