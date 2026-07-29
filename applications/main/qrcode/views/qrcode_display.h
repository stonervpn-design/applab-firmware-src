#pragma once

#include <gui/view.h>

typedef struct QRCodeDisplay QRCodeDisplay;

QRCodeDisplay* qrcode_display_alloc(void);

void qrcode_display_free(QRCodeDisplay* instance);

View* qrcode_display_get_view(QRCodeDisplay* instance);

/** Encode `text` (auto-version, MODE_BYTE / ECC_MEDIUM) and store the result
 *  for rendering. Safe to call from the app thread before switching views. */
void qrcode_display_set_payload(QRCodeDisplay* instance, const char* text);
