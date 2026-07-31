#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { NfcShareRoleSend, NfcShareRoleReceive } NfcShareRole;
typedef enum {
    NfcShareStateInit, NfcShareStateWaiting, NfcShareStateRunning,
    NfcShareStateDoneOk, NfcShareStateFail
} NfcShareState;
typedef struct {
    NfcShareState state;
    char     name[40];
    uint32_t file_size;
    uint32_t bytes_done;
    uint32_t blocks_total;
    uint32_t blocks_done;
    uint32_t kbps_x10;
    uint8_t  err;               // 0 none, 1 io, 2 md5, 3 radio, 4 toobig
} NfcShareStatus;

typedef struct NfcShareEngine NfcShareEngine;
NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path);
void nfc_share_engine_start(NfcShareEngine* e);
void nfc_share_engine_stop(NfcShareEngine* e);
void nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out);
void nfc_share_engine_free(NfcShareEngine* e);
