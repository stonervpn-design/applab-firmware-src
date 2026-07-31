#pragma once
#include <gui/view.h>
#include "../nfc_share_engine.h"
typedef struct NfcShareProgressView NfcShareProgressView;
NfcShareProgressView* nfc_share_progress_view_alloc(void);
void nfc_share_progress_view_free(NfcShareProgressView* v);
View* nfc_share_progress_view_get_view(NfcShareProgressView* v);
void nfc_share_progress_view_update(NfcShareProgressView* v, const NfcShareStatus* s, bool sending);
