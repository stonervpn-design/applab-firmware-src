#include "nfc_share_engine.h"
#include <furi.h>
#include <string.h>
struct NfcShareEngine { NfcShareRole role; char path[256]; NfcShareStatus st; };
NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path) {
    NfcShareEngine* e = malloc(sizeof(NfcShareEngine));
    memset(e, 0, sizeof(*e));
    e->role = role;
    strncpy(e->path, path ? path : "", sizeof(e->path) - 1);
    e->st.state = NfcShareStateWaiting;
    return e;
}
void nfc_share_engine_start(NfcShareEngine* e) { (void)e; }
void nfc_share_engine_stop(NfcShareEngine* e) { (void)e; }
void nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out) { *out = e->st; }
void nfc_share_engine_free(NfcShareEngine* e) { free(e); }
