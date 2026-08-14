#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef PUBLIC_BUILD
#define NRF24_JAM_MODE_COUNT 10 /* + BLE Reactive (private) */
#else
#define NRF24_JAM_MODE_COUNT 9 /* public: no reactive */
#endif

/* Simple mode-list jammer model (Bruce-base + concentration/noise improvements). */
typedef struct {
    int mode; /* 0..NRF24_JAM_MODE_COUNT-1, selected/active mode */
    bool running; /* currently jamming */
    bool hardware_ok;
} Nrf24JamModel;

typedef enum {
    Nrf24JamEventToggle = 1, /* OK: start / stop */
    Nrf24JamEventNext, /* Down: next mode (live) */
    Nrf24JamEventPrev, /* Up: previous mode (Cardputer only; StickS3 has no Up) */
} Nrf24JamEvent;

/* Shared mode metadata (single source of truth for view + worker). */
const char* nrf24_jam_mode_name(int mode);
bool nrf24_jam_mode_is_flood(int mode); /* true = Turbo noise-flood, false = CW */
bool nrf24_jam_mode_is_reactive(int mode); /* true = RPD reactive BLE (mode 9) */

View* nrf24_jam_view_alloc(void);
void nrf24_jam_view_free(View* view);
