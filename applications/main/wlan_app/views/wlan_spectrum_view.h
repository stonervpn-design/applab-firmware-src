#pragma once
#include <gui/view.h>
#include "../wlan_spectrum.h"

typedef enum { SpecFilterAll, SpecFilterVuln, SpecFilterHidden } WlanSpecFilter;

/** Custom events emitted by the spectrum view input callback to the scene.
 *  Shared with scene_spectrum.c (values chosen to not collide with the shared
 *  wlan_view_events.h enum, which uses 100..202). */
typedef enum {
    SpectrumEvtLeft = 0x5001,
    SpectrumEvtRight,
    SpectrumEvtOk,
    SpectrumEvtFilter,
} SpectrumEvt;

typedef struct {
    const WlanSpectrum* spec;
    uint8_t cur_channel;
    bool locked;
    bool paused;
    WlanSpecFilter filter;
    uint16_t vuln_mask;
    uint16_t hidden_mask;
} WlanSpectrumViewModel;

View* wlan_spectrum_view_alloc(void);
void wlan_spectrum_view_free(View* v);
