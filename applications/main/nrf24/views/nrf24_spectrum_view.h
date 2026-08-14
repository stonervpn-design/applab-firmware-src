#pragma once

#include <gui/view.h>

#define NRF24_SPECTRUM_CHANNELS 80

typedef enum {
    Nrf24SpectrumEventCursorLeft = 1,
    Nrf24SpectrumEventCursorRight,
    Nrf24SpectrumEventToggleMaxHold,
    Nrf24SpectrumEventDwellCycle,
    Nrf24SpectrumEventExport,
} Nrf24SpectrumEvent;

typedef struct {
    uint8_t levels[NRF24_SPECTRUM_CHANNELS];
    uint16_t sweep_count;
    bool running;
    bool hardware_ok;
    uint8_t peaks[NRF24_SPECTRUM_CHANNELS]; /* max-hold */
    bool max_hold; /* toggle */
    uint8_t cursor_ch; /* 0..NRF24_SPECTRUM_CHANNELS-1 (Task 5) */
    bool cursor_on; /* show cursor readout (Task 5) */
    uint16_t dwell_us; /* current dwell (Task 5) */
} Nrf24SpectrumModel;

View* nrf24_spectrum_view_alloc(void);
void nrf24_spectrum_view_free(View* view);
