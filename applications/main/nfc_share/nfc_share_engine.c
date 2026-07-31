#include "nfc_share_engine.h"
#include "nfc_share_proto.h"
#include "furi_hal/st25r3916/st25r3916_nfca.h"
#include <furi.h>
#include <storage/storage.h>
#include <mbedtls/md5.h>
#include <string.h>
#include <stdlib.h>

/* ── Send role: the M5 emulates a card, ANNOUNCEs the selected SD file, and
 * serves DATA blocks streamed from storage on demand — adapted from the
 * hardware-proven listener worker in
 * applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c (Phase 3).
 * The receive role is still a stub (Task 4); it just stays Waiting. */

/* Flipper Share transport card identity (nfc_transport_config.h upstream). */
static const uint8_t kShareUid[7] = {0xF5, 0x0F, 0x5A, 0xAE, 0x00, 0x00, 0x01};
#define SHARE_ATQA  0x0044u
#define SHARE_SAK   0x00u
#define SHARE_TX_ID 0x5Au

#define NFC_SHARE_MAX_FILE_SIZE (16u * 1024u * 1024u) /* 16 MB guard, brief step 3 */

struct NfcShareEngine {
    NfcShareRole role;
    char path[256];
    NfcShareStatus st;

    Storage* storage;
    File* file;
    FuriThread* worker;
    volatile bool run;
    FuriMutex* lock;
    bool listening; /* true once st25r3916_nfca_listen_start() succeeded */

    /* sender-only state (worker-thread-owned once started) */
    uint8_t tx_id;
    uint32_t total_blocks;
    bool locked; /* a valid REQUEST seen -> stop announcing */
    bool pending; /* blocks still queued to send */
    uint32_t send_next; /* next block to send */
    uint32_t send_last; /* last requested block (inclusive) */
    uint8_t md5[16];
};

/* Streaming md5 over the open file; seeks to 0 first and restores afterwards
 * so build_data_from_file() can keep reading from the same handle. */
static bool compute_file_md5(File* f, uint32_t size, uint8_t out[16]) {
    if(!storage_file_seek(f, 0, true)) return false;
    mbedtls_md5_context c;
    mbedtls_md5_init(&c);
    mbedtls_md5_starts(&c);
    uint8_t buf[512];
    uint32_t left = size;
    while(left) {
        uint32_t want = left > sizeof(buf) ? sizeof(buf) : left;
        size_t got = storage_file_read(f, buf, want);
        if(got == 0) {
            mbedtls_md5_free(&c);
            return false;
        }
        mbedtls_md5_update(&c, buf, (uint32_t)got);
        left -= (uint32_t)got;
    }
    mbedtls_md5_finish(&c, out);
    mbedtls_md5_free(&c);
    return storage_file_seek(f, 0, true);
}

/* Build one DATA frame with the block's bytes read straight from SD (zero-pad
 * the tail block). */
static uint8_t build_data_from_file(NfcShareEngine* e, uint32_t block, uint8_t* out) {
    uint8_t data[NSH_DATA_LENGTH];
    memset(data, 0, sizeof(data));
    uint32_t off = block * NSH_DATA_LENGTH;
    uint32_t valid = (off < e->st.file_size) ? (e->st.file_size - off) : 0;
    if(valid > NSH_DATA_LENGTH) valid = NSH_DATA_LENGTH;
    if(valid) {
        storage_file_seek(e->file, off, true);
        storage_file_read(e->file, data, valid);
    }
    return (uint8_t)nfc_share_build_data(out, e->tx_id, block, data, valid);
}

/* Tight exchange loop: receive a poller frame, parse it, and answer within FWT
 * with a DATA block (if requested), else a keep-alive (once locked) or an
 * ANNOUNCE (before lock). Same control flow as nfc_tp_listener_worker in
 * bus_tools_scene_st25r_nfc.c, with the frame builders swapped for the codec
 * in nfc_share_proto.c and the payload sourced from the open SD file. */
static int32_t send_worker(void* ctx) {
    NfcShareEngine* e = ctx;
    uint8_t rx[64], announce[NSH_ANNOUNCE_FRAME_LEN], data[NSH_DATA_FRAME_LEN];
    uint8_t ka = NFC_TP_HDR_POLL;
    nfc_share_build_announce(announce, e->tx_id, e->st.name, e->st.file_size, e->md5);

    while(e->run) {
        St25rListenState state = St25rListenIdle;
        uint8_t raw = 0;
        st25r3916_nfca_listen_poll(&state, &raw);
        if(state != St25rListenRx) {
            furi_delay_ms(1);
            continue;
        }

        uint8_t n = 0;
        if(st25r3916_nfca_listen_rx(rx, sizeof(rx), &n) != St25rNfcaOk || n == 0) continue;

        uint32_t rs, re;
        if(nfc_share_parse_request(rx, n, e->tx_id, &rs, &re)) {
            uint32_t first = rs / NSH_DATA_LENGTH;
            uint32_t last = re ? (re - 1) / NSH_DATA_LENGTH : 0;
            if(first >= e->total_blocks) first = e->total_blocks - 1;
            if(last >= e->total_blocks) last = e->total_blocks - 1;
            e->send_next = first;
            e->send_last = last;
            e->pending = (first <= last);
            e->locked = true;
        }

        const uint8_t* txp;
        uint8_t txlen;
        bool is_data = false;
        if(e->pending && e->send_next < e->total_blocks) {
            txlen = build_data_from_file(e, e->send_next, data);
            txp = data;
            is_data = true;
            if(e->send_next++ >= e->send_last) e->pending = false;
        } else if(e->locked) {
            txp = &ka;
            txlen = 1;
        } else {
            txp = announce;
            txlen = NSH_ANNOUNCE_FRAME_LEN;
        }

        bool txok = (st25r3916_nfca_listen_tx(txp, txlen) == St25rNfcaOk);
        if(is_data && txok) {
            furi_mutex_acquire(e->lock, FuriWaitForever);
            e->st.blocks_done = e->send_next;
            e->st.bytes_done = e->send_next * NSH_DATA_LENGTH;
            if(e->st.bytes_done > e->st.file_size) e->st.bytes_done = e->st.file_size;
            e->st.state = (e->st.blocks_done >= e->total_blocks) ? NfcShareStateDoneOk :
                                                                    NfcShareStateRunning;
            furi_mutex_release(e->lock);
        }
    }
    return 0;
}

NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path) {
    NfcShareEngine* e = malloc(sizeof(NfcShareEngine));
    memset(e, 0, sizeof(*e));
    e->role = role;
    strncpy(e->path, path ? path : "", sizeof(e->path) - 1);
    e->st.state = NfcShareStateWaiting;
    e->storage = furi_record_open(RECORD_STORAGE);
    e->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    return e;
}

void nfc_share_engine_start(NfcShareEngine* e) {
    if(!e) return;
    if(e->role != NfcShareRoleSend) {
        /* Receive role: Task 4 implements the poller. Stay Waiting for now. */
        e->st.state = NfcShareStateWaiting;
        return;
    }

    e->file = storage_file_alloc(e->storage);
    if(!storage_file_open(e->file, e->path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
        e->st.state = NfcShareStateFail;
        e->st.err = 1;
        return;
    }

    uint64_t size64 = storage_file_size(e->file);
    if(size64 == 0 || size64 > NFC_SHARE_MAX_FILE_SIZE) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
        e->st.state = NfcShareStateFail;
        e->st.err = 4;
        return;
    }
    e->st.file_size = (uint32_t)size64;
    e->total_blocks = (e->st.file_size + NSH_DATA_LENGTH - 1u) / NSH_DATA_LENGTH;
    e->st.blocks_total = e->total_blocks;

    /* Leaf name only (strip any directory component) into st.name. */
    const char* leaf = strrchr(e->path, '/');
    leaf = leaf ? leaf + 1 : e->path;
    strncpy(e->st.name, leaf, sizeof(e->st.name) - 1);
    e->st.name[sizeof(e->st.name) - 1] = '\0';

    if(!compute_file_md5(e->file, e->st.file_size, e->md5)) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
        e->st.state = NfcShareStateFail;
        e->st.err = 1;
        return;
    }

    e->tx_id = SHARE_TX_ID;
    e->locked = false;
    e->pending = false;
    e->send_next = 0;
    e->send_last = 0;

    if(st25r3916_nfca_listen_start(kShareUid, sizeof(kShareUid), SHARE_ATQA, SHARE_SAK) !=
       St25rNfcaOk) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
        e->st.state = NfcShareStateFail;
        e->st.err = 3;
        return;
    }
    e->listening = true;

    e->st.state = NfcShareStateWaiting;
    e->run = true;
    e->worker = furi_thread_alloc_ex("NfcShareSend", 4096, send_worker, e);
    furi_thread_start(e->worker);
}

void nfc_share_engine_stop(NfcShareEngine* e) {
    if(!e) return;
    if(e->worker) {
        e->run = false;
        furi_thread_join(e->worker);
        furi_thread_free(e->worker);
        e->worker = NULL;
    }
    if(e->listening) {
        st25r3916_nfca_listen_stop();
        e->listening = false;
    }
    if(e->file) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
    }
}

void nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out) {
    if(!e || !out) return;
    if(e->lock) furi_mutex_acquire(e->lock, FuriWaitForever);
    *out = e->st;
    if(e->lock) furi_mutex_release(e->lock);
}

void nfc_share_engine_free(NfcShareEngine* e) {
    if(!e) return;
    if(e->lock) furi_mutex_free(e->lock);
    furi_record_close(RECORD_STORAGE);
    free(e);
}
