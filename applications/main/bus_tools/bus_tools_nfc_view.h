#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct {
    int init_ok; /* St25rNfcaStatus of init (0 = ok) */
    int last_status; /* St25rNfcaStatus of the most recent poll */
    uint8_t uid[10];
    uint8_t uid_len;
    uint16_t atqa;
    uint8_t sak;
    uint32_t reads; /* successful reads so far */
} BusToolsNfcViewModel;

View* bus_tools_nfc_view_alloc(void);
void bus_tools_nfc_view_free(View* v);
