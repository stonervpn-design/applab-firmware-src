#include "genie_seq.h"
#include <string.h>

// ---------------- pure core (big-endian buffer ops; no storage) ----------------

static uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void genie_seq_buf_init(uint8_t* buf, uint32_t code_lo) {
    memset(buf, 0, GENIE_SEQ_FILE_SIZE);
    wr16(buf + GENIE_SEQ_OFF_MAGIC, GENIE_SEQ_MAGIC);
    wr16(buf + GENIE_SEQ_OFF_VERSION, (uint16_t)((GENIE_SEQ_VER_MAJOR << 8) | GENIE_SEQ_VER_MINOR));
    wr32(buf + GENIE_SEQ_OFF_SN, code_lo);
    wr16(buf + GENIE_SEQ_OFF_LASTS, 0);
    wr16(buf + GENIE_SEQ_OFF_RECCNT, 0);
}

bool genie_seq_buf_valid(const uint8_t* buf) {
    if(be16(buf + GENIE_SEQ_OFF_MAGIC) != GENIE_SEQ_MAGIC) return false;
    return (be16(buf + GENIE_SEQ_OFF_VERSION) >> 8) <= GENIE_SEQ_VER_MAJOR;
}
uint16_t genie_seq_buf_rec_count(const uint8_t* buf) {
    return be16(buf + GENIE_SEQ_OFF_RECCNT);
}
uint16_t genie_seq_buf_last_sent(const uint8_t* buf) {
    return be16(buf + GENIE_SEQ_OFF_LASTS);
}
uint32_t genie_seq_buf_sn(const uint8_t* buf) {
    return be32(buf + GENIE_SEQ_OFF_SN);
}
uint32_t genie_seq_buf_code_at(const uint8_t* buf, uint16_t index) {
    return be32(buf + GENIE_SEQ_OFF_DATA + (uint32_t)index * 4u);
}
void genie_seq_buf_set_last_sent(uint8_t* buf, uint16_t idx) {
    wr16(buf + GENIE_SEQ_OFF_LASTS, idx);
}

uint32_t genie_seq_buf_append(uint8_t* buf, uint32_t code_hi) {
    // Matches ARF genie_save_bin: append at REC_COUNT, guard the single target slot.
    uint16_t count = be16(buf + GENIE_SEQ_OFF_RECCNT);
    uint8_t* slot = buf + GENIE_SEQ_OFF_DATA + (uint32_t)count * 4u;
    uint32_t existing = be32(slot);
    if(existing != 0 && existing != code_hi) {
        return (count != 0xFFFF) ? count : GENIE_SEQ_FULL;
    }
    wr32(slot, code_hi);
    if(count != 0xFFFF) {
        count++;
        wr16(buf + GENIE_SEQ_OFF_RECCNT, count);
        return count;
    }
    return GENIE_SEQ_FULL;
}

#ifndef GENIE_SEQ_HOST_TEST
// ---------------- device storage wrappers (seek + patch; never loads 262KB) ----------------
#include <furi.h>
#include <storage/storage.h>

void genie_seq_path_for(const char* folder, uint32_t code_lo, char* out, unsigned n) {
    snprintf(out, n, "%s/%08lX.gne", folder, (unsigned long)code_lo);
}

static uint16_t sf_r16(File* f) {
    uint8_t b[2] = {0};
    storage_file_read(f, b, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
}
static uint32_t sf_r32(File* f) {
    uint8_t b[4] = {0};
    storage_file_read(f, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static bool sf_w16(File* f, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return storage_file_write(f, b, 2) == 2;
}
static bool sf_w32(File* f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return storage_file_write(f, b, 4) == 4;
}

static void genie_seq_create(Storage* storage, const char* path, uint32_t code_lo) {
    File* f = storage_file_alloc(storage);
    if(f && storage_file_open(f, path, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS)) {
        sf_w16(f, GENIE_SEQ_MAGIC);
        sf_w16(f, (uint16_t)((GENIE_SEQ_VER_MAJOR << 8) | GENIE_SEQ_VER_MINOR));
        sf_w32(f, code_lo);
        sf_w16(f, 0); // last_sent
        sf_w16(f, 0); // rec_count
        sf_w32(f, 0); // reserved
        uint8_t zeros[256] = {0};
        for(int i = 0; i < 1024; i++) storage_file_write(f, zeros, 256); // 262144 record bytes
    }
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
}

uint32_t genie_seq_append_code(const char* folder, uint64_t code) {
    uint32_t code_lo = genie_seq_code_lo(code);
    uint32_t code_hi = genie_seq_code_hi(code);
    uint32_t result = 0;
    char path[128];
    genie_seq_path_for(folder, code_lo, path, sizeof(path));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    do {
        if(!storage) break;
        if(!storage_dir_exists(storage, folder)) storage_simply_mkdir(storage, folder);
        if(!storage_file_exists(storage, path)) genie_seq_create(storage, path, code_lo);
        f = storage_file_alloc(storage);
        if(!f || !storage_file_open(f, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) break;

        storage_file_seek(f, GENIE_SEQ_OFF_VERSION, true);
        if((sf_r16(f) >> 8) > GENIE_SEQ_VER_MAJOR) break;
        storage_file_seek(f, GENIE_SEQ_OFF_SN, true);
        if(sf_r32(f) != code_lo) break; // wrong remote for this file
        storage_file_seek(f, GENIE_SEQ_OFF_RECCNT, true);
        uint16_t count = sf_r16(f);

        storage_file_seek(f, GENIE_SEQ_OFF_DATA + (uint32_t)count * 4u, true);
        uint32_t existing = sf_r32(f);
        if(existing != 0 && existing != code_hi) {
            result = (count != 0xFFFF) ? count : GENIE_SEQ_FULL;
            break;
        }
        storage_file_seek(f, GENIE_SEQ_OFF_DATA + (uint32_t)count * 4u, true);
        if(!sf_w32(f, code_hi)) break;
        if(count != 0xFFFF) {
            count++;
            storage_file_seek(f, GENIE_SEQ_OFF_RECCNT, true);
            if(!sf_w16(f, count)) break;
            result = count;
        } else {
            result = GENIE_SEQ_FULL;
        }
    } while(false);

    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    furi_record_close(RECORD_STORAGE);
    return result;
}

bool genie_seq_stat(
    const char* folder,
    uint32_t code_lo,
    uint32_t* sn,
    uint16_t* last_sent,
    uint16_t* rec_count) {
    char path[128];
    genie_seq_path_for(folder, code_lo, path, sizeof(path));
    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    do {
        if(!storage || !storage_file_exists(storage, path)) break;
        f = storage_file_alloc(storage);
        if(!f || !storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        storage_file_seek(f, GENIE_SEQ_OFF_VERSION, true);
        if((sf_r16(f) >> 8) > GENIE_SEQ_VER_MAJOR) break;
        storage_file_seek(f, GENIE_SEQ_OFF_SN, true);
        uint32_t s = sf_r32(f);
        storage_file_seek(f, GENIE_SEQ_OFF_LASTS, true);
        uint16_t ls = sf_r16(f);
        storage_file_seek(f, GENIE_SEQ_OFF_RECCNT, true);
        uint16_t rc = sf_r16(f);
        if(sn) *sn = s;
        if(last_sent) *last_sent = ls;
        if(rec_count) *rec_count = rc;
        ok = true;
    } while(false);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

uint64_t genie_seq_code_at_path(const char* path, uint16_t index) {
    uint64_t out = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    do {
        if(!storage || !storage_file_exists(storage, path)) break;
        f = storage_file_alloc(storage);
        if(!f || !storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        storage_file_seek(f, GENIE_SEQ_OFF_SN, true);
        uint32_t lo = sf_r32(f);
        storage_file_seek(f, GENIE_SEQ_OFF_DATA + (uint32_t)index * 4u, true);
        uint32_t hi = sf_r32(f);
        out = ((uint64_t)hi << 32) | lo;
    } while(false);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    furi_record_close(RECORD_STORAGE);
    return out;
}

bool genie_seq_stat_path(
    const char* path,
    uint32_t* sn,
    uint16_t* last_sent,
    uint16_t* rec_count) {
    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    do {
        if(!storage || !storage_file_exists(storage, path)) break;
        f = storage_file_alloc(storage);
        if(!f || !storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        storage_file_seek(f, GENIE_SEQ_OFF_VERSION, true);
        if((sf_r16(f) >> 8) > GENIE_SEQ_VER_MAJOR) break;
        storage_file_seek(f, GENIE_SEQ_OFF_SN, true);
        uint32_t s = sf_r32(f);
        storage_file_seek(f, GENIE_SEQ_OFF_LASTS, true);
        uint16_t ls = sf_r16(f);
        storage_file_seek(f, GENIE_SEQ_OFF_RECCNT, true);
        uint16_t rc = sf_r16(f);
        if(sn) *sn = s;
        if(last_sent) *last_sent = ls;
        if(rec_count) *rec_count = rc;
        ok = true;
    } while(false);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool genie_seq_set_last_sent_path(const char* path, uint16_t idx) {
    bool ok = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    do {
        if(!storage || !storage_file_exists(storage, path)) break;
        f = storage_file_alloc(storage);
        if(!f || !storage_file_open(f, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) break;
        storage_file_seek(f, GENIE_SEQ_OFF_VERSION, true);
        if((sf_r16(f) >> 8) > GENIE_SEQ_VER_MAJOR) break;
        storage_file_seek(f, GENIE_SEQ_OFF_LASTS, true);
        ok = sf_w16(f, idx);
    } while(false);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    furi_record_close(RECORD_STORAGE);
    return ok;
}
#endif // GENIE_SEQ_HOST_TEST
