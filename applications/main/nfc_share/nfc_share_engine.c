#include "nfc_share_engine.h"
#include "nfc_share_proto.h"
#include "nfc_share_i.h"
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
 *
 * Receive role (Task 4): the M5 is the poller, pulling a file FROM a sender
 * (e.g. a real Flipper Zero running Flipper Share NFC). Adapted from the
 * hardware-proven nfc_rx_poller_worker / rx_handle / rx_build_request in the
 * same reference file, with one substantive change: DATA blocks are written
 * straight to their SD offset (streaming) instead of into a RAM file buffer,
 * and the file is re-read from SD at the end to md5-verify. */

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

    /* receiver-only state (worker-thread-owned once started) */
    uint8_t rx_txid;
    uint32_t blocks_got;
    uint8_t* bitmap;
    uint8_t exp_md5[16];
    char dest[NFC_SHARE_PATH_MAX];
    uint32_t loop; /* exchange counter, for request pacing */

    uint32_t t0; /* tick at first transition into NfcShareStateRunning, for kbps calc */
};

/* Throughput helper: stamp t0 once when the transfer first starts running,
 * then derive kbps_x10 (tenths of KB/s) from bytes_done and elapsed ms.
 * Caller must hold e->lock. bytes_done is capped well under 16 MB, so the
 * intermediate product is done in 64-bit to avoid uint32 overflow. */
static void update_throughput(NfcShareEngine* e) {
    uint32_t now = furi_get_tick();
    if(e->t0 == 0) e->t0 = now;
    uint32_t elapsed_ms = now - e->t0;
    if(elapsed_ms == 0) elapsed_ms = 1;
    e->st.kbps_x10 =
        (uint32_t)((uint64_t)e->st.bytes_done * 10000ULL / elapsed_ms / 1024ULL);
}

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
            update_throughput(e);
            furi_mutex_release(e->lock);
        }
    }
    return 0;
}

/* ── Receive role helpers ─────────────────────────────────────────────── */

static bool bitmap_have(NfcShareEngine* e, uint32_t b) {
    return e->bitmap && (e->bitmap[b >> 3] & (uint8_t)(1u << (b & 7)));
}
static void bitmap_mark(NfcShareEngine* e, uint32_t b) {
    if(e->bitmap) e->bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
}
static uint32_t bitmap_first_missing(NfcShareEngine* e) {
    for(uint32_t b = 0; b < e->total_blocks; b++)
        if(!bitmap_have(e, b)) return b;
    return e->total_blocks;
}

/* Resolve a non-clobbering destination path under NFC_SHARE_RECV_FOLDER using
 * the pure dedupe helper + a storage existence check: never overwrite an
 * existing file, name it "<leaf>_1", "<leaf>_2", ... instead. */
static void resolve_dest(Storage* s, const char* leaf, char* out, size_t out_sz) {
    storage_common_mkdir(s, NFC_SHARE_RECV_FOLDER); /* ok if it already exists */
    for(unsigned k = 0; k < 1000; k++) {
        char name[64];
        nfc_share_dedup_name(leaf, k, name, sizeof(name));
        snprintf(out, out_sz, "%s/%s", NFC_SHARE_RECV_FOLDER, name);
        if(storage_common_stat(s, out, NULL) != FSE_OK) return; /* free slot */
    }
}

/* Reset per-transfer receive state and release any buffers/handle from a
 * previous (or aborted) transfer, without touching e->run or the thread. */
static void rx_reset(NfcShareEngine* e) {
    if(e->file) {
        storage_file_close(e->file);
        storage_file_free(e->file);
        e->file = NULL;
    }
    free(e->bitmap);
    e->bitmap = NULL;
    e->total_blocks = 0;
    e->blocks_got = 0;
}

/* Dispatch one sender response frame (resp/rn include tp_hdr + trailing
 * transport CRC-A) — same control flow as rx_handle() in
 * bus_tools_scene_st25r_nfc.c, adapted to stream DATA blocks straight to SD
 * instead of into a RAM file buffer. Returns true once the transfer reaches a
 * terminal state (DoneOk or Fail). */
static bool recv_handle(NfcShareEngine* e, const uint8_t* resp, uint8_t rn) {
    uint8_t txid;
    char name[37];
    uint32_t fsize;
    uint8_t md5[16];
    if(nfc_share_parse_announce(resp, rn, &txid, name, &fsize, md5)) {
        if(e->st.state != NfcShareStateWaiting) return false; /* already locked on */

        if(fsize == 0 || fsize > NFC_SHARE_MAX_FILE_SIZE) {
            furi_mutex_acquire(e->lock, FuriWaitForever);
            e->st.state = NfcShareStateFail;
            e->st.err = 4;
            furi_mutex_release(e->lock);
            return true;
        }

        /* Sanitize the announced name to a leaf: substring after the last
         * '/' or '\\'. */
        const char* leaf = name;
        const char* slash = strrchr(name, '/');
        const char* bslash = strrchr(name, '\\');
        if(bslash && (!slash || bslash > slash)) slash = bslash;
        if(slash) leaf = slash + 1;
        if(*leaf == '\0') leaf = "nfc_share_file";

        resolve_dest(e->storage, leaf, e->dest, sizeof(e->dest));

        e->file = storage_file_alloc(e->storage);
        if(!storage_file_open(e->file, e->dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_close(e->file);
            storage_file_free(e->file);
            e->file = NULL;
            furi_mutex_acquire(e->lock, FuriWaitForever);
            e->st.state = NfcShareStateFail;
            e->st.err = 1;
            furi_mutex_release(e->lock);
            return true;
        }

        uint32_t blocks = (fsize + NSH_DATA_LENGTH - 1u) / NSH_DATA_LENGTH;
        uint8_t* bm = calloc((blocks + 7u) / 8u, 1);
        if(!bm) {
            storage_file_close(e->file);
            storage_file_free(e->file);
            e->file = NULL;
            furi_mutex_acquire(e->lock, FuriWaitForever);
            e->st.state = NfcShareStateFail;
            e->st.err = 1;
            furi_mutex_release(e->lock);
            return true;
        }
        e->bitmap = bm;
        e->total_blocks = blocks;
        e->blocks_got = 0;
        e->rx_txid = txid;
        memcpy(e->exp_md5, md5, 16);

        furi_mutex_acquire(e->lock, FuriWaitForever);
        e->st.file_size = fsize;
        e->st.blocks_total = blocks;
        e->st.bytes_done = 0;
        e->st.blocks_done = 0;
        strncpy(e->st.name, leaf, sizeof(e->st.name) - 1);
        e->st.name[sizeof(e->st.name) - 1] = '\0';
        e->st.state = NfcShareStateRunning;
        e->t0 = furi_get_tick(); /* stamp the Running-transition start for kbps */
        e->st.kbps_x10 = 0;
        furi_mutex_release(e->lock);
        return false;
    }

    uint32_t block, dn;
    const uint8_t* dp;
    if(nfc_share_parse_data(resp, rn, e->rx_txid, &block, &dp, &dn) &&
       e->st.state == NfcShareStateRunning) {
        if(block < e->total_blocks && !bitmap_have(e, block)) {
            uint32_t off = block * NSH_DATA_LENGTH;
            uint32_t valid = e->st.file_size - off;
            if(valid > NSH_DATA_LENGTH) valid = NSH_DATA_LENGTH;
            if(valid > dn) valid = dn;
            if(!storage_file_seek(e->file, off, true)) {
                storage_file_close(e->file);
                storage_file_free(e->file);
                e->file = NULL;
                furi_mutex_acquire(e->lock, FuriWaitForever);
                e->st.state = NfcShareStateFail;
                e->st.err = 1;
                furi_mutex_release(e->lock);
                return true;
            }
            if(storage_file_write(e->file, dp, valid) == valid) {
                bitmap_mark(e, block);
                e->blocks_got++;
                furi_mutex_acquire(e->lock, FuriWaitForever);
                e->st.bytes_done += valid;
                if(e->st.bytes_done > e->st.file_size) e->st.bytes_done = e->st.file_size;
                e->st.blocks_done = e->blocks_got;
                update_throughput(e);
                furi_mutex_release(e->lock);
            }
            if(e->blocks_got >= e->total_blocks) {
                /* Finalize: close the write handle, reopen read-only, and
                 * re-read from SD to md5-verify (never trust the RAM path
                 * we just streamed through). */
                storage_file_close(e->file);
                storage_file_free(e->file);
                e->file = storage_file_alloc(e->storage);
                uint8_t got_md5[16];
                bool ok = storage_file_open(e->file, e->dest, FSAM_READ, FSOM_OPEN_EXISTING) &&
                          compute_file_md5(e->file, e->st.file_size, got_md5);
                furi_mutex_acquire(e->lock, FuriWaitForever);
                if(!ok) {
                    e->st.state = NfcShareStateFail;
                    e->st.err = 1;
                } else if(memcmp(got_md5, e->exp_md5, 16) != 0) {
                    /* md5 mismatch: keep the file on disk for inspection. */
                    e->st.state = NfcShareStateFail;
                    e->st.err = 2;
                } else {
                    e->st.state = NfcShareStateDoneOk;
                }
                furi_mutex_release(e->lock);
                storage_file_close(e->file);
                storage_file_free(e->file);
                e->file = NULL;
                return true;
            }
        }
    }
    return false;
}

/* Poller worker: the M5 actively fields the NFC-A card, pulling a file FROM
 * the sender via periodic REQUESTs and mostly keep-alive polls (the sender
 * streams DATA on polls, not on every REQUEST) — same control flow as the
 * hardware-proven nfc_rx_poller_worker() in bus_tools_scene_st25r_nfc.c. */
static int32_t recv_worker(void* ctx) {
    NfcShareEngine* e = ctx;
    if(st25r3916_nfca_init() != St25rNfcaOk) {
        furi_mutex_acquire(e->lock, FuriWaitForever);
        e->st.state = NfcShareStateFail;
        e->st.err = 3;
        furi_mutex_release(e->lock);
        return 0;
    }

    bool activated = false;
    uint8_t resp[255];
    uint8_t txf[64];
    uint8_t ka = NFC_TP_HDR_POLL;

    while(e->run) {
        if(!activated) {
            St25rNfcaTag tag;
            if(st25r3916_nfca_poll(&tag) == St25rNfcaOk) {
                activated = true;
            } else {
                furi_delay_ms(2);
                continue;
            }
        }

        e->loop++;
        const uint8_t* txp = &ka;
        uint8_t txlen = 1;
        if(e->st.state == NfcShareStateRunning) {
            uint32_t fm = bitmap_first_missing(e);
            if(fm < e->total_blocks && (e->loop % 6u == 0u)) {
                txlen = (uint8_t)nfc_share_build_request(
                    txf, e->rx_txid, fm * NSH_DATA_LENGTH, e->st.file_size);
                txp = txf;
            }
        }

        uint8_t rn = 0;
        if(st25r3916_nfca_poller_txrx(txp, txlen, resp, sizeof(resp), &rn) != St25rNfcaOk) {
            activated = false; /* lost the card -> re-activate next loop */
            continue;
        }

        recv_handle(e, resp, rn);
        if(e->st.state == NfcShareStateDoneOk || e->st.state == NfcShareStateFail) {
            furi_delay_ms(10); /* transfer finished (ok or fail): idle every loop */
        }
    }

    st25r3916_nfca_deinit();
    rx_reset(e);
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
        /* Receive role: poll for a sender and pull its file to SD. */
        e->st.state = NfcShareStateWaiting;
        e->run = true;
        e->worker = furi_thread_alloc_ex("NfcShareRecv", 4096, recv_worker, e);
        furi_thread_start(e->worker);
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
