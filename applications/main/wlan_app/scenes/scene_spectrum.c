#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_spectrum.h"
#include "../views/wlan_spectrum_view.h"
#include <esp_wifi.h>
#include <string.h>

#define SPEC_MIN_CH 1
#define SPEC_MAX_CH 13

static WlanSpectrum s_spec;
static volatile uint32_t s_dwell_count;
static volatile int8_t s_dwell_peak;
static uint8_t s_cur_ch;
static bool s_running;

static void spectrum_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    s_dwell_count++;
    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
    if(rssi > s_dwell_peak) s_dwell_peak = rssi;
}

// Background scan to tag VULN (open/WEP) + HIDDEN channels.
static void spectrum_tag_channels(WlanApp* app) {
    uint16_t vuln = 0, hidden = 0;
    wifi_ap_record_t* recs = NULL;
    uint16_t n = 0;
    wlan_hal_scan(&recs, &n, WLAN_APP_MAX_APS);
    for(uint16_t i = 0; i < n; i++) {
        uint8_t ch = recs[i].primary;
        if(ch < 1 || ch > SPEC_MAX_CH) continue;
        if(recs[i].authmode == WIFI_AUTH_OPEN || recs[i].authmode == WIFI_AUTH_WEP) {
            vuln |= (uint16_t)(1u << (ch - 1));
        }
        if(recs[i].ssid[0] == '\0') hidden |= (uint16_t)(1u << (ch - 1));
    }
    if(recs) free(recs);
    with_view_model(
        app->view_spectrum, WlanSpectrumViewModel * m,
        { m->vuln_mask = vuln; m->hidden_mask = hidden; }, true);
}

void wlan_app_scene_spectrum_on_enter(void* context) {
    WlanApp* app = context;
    wlan_spectrum_reset(&s_spec);
    s_dwell_count = 0;
    s_dwell_peak = -128;
    s_cur_ch = SPEC_MIN_CH;
    s_running = true;

    with_view_model(
        app->view_spectrum, WlanSpectrumViewModel * m,
        {
            m->spec = &s_spec;
            m->cur_channel = s_cur_ch;
            m->locked = false;
            m->paused = false;
            m->filter = SpecFilterAll;
            m->vuln_mask = 0;
            m->hidden_mask = 0;
        },
        true);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    spectrum_tag_channels(app); // one-shot scan before promiscuous
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, spectrum_cb);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSpectrum);
}

bool wlan_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        bool locked = false, paused = false;
        with_view_model(
            app->view_spectrum, WlanSpectrumViewModel * m,
            {
                locked = m->locked;
                paused = m->paused;
            },
            false);
        switch(event.event) {
        case SpectrumEvtRight:
        case SpectrumEvtLeft: {
            // Toggle lock; when locking, stay on current channel.
            bool nl = !locked;
            with_view_model(
                app->view_spectrum, WlanSpectrumViewModel * m, { m->locked = nl; }, true);
            consumed = true;
            break;
        }
        case SpectrumEvtOk: {
            bool np = !paused;
            with_view_model(
                app->view_spectrum, WlanSpectrumViewModel * m, { m->paused = np; }, true);
            consumed = true;
            break;
        }
        case SpectrumEvtFilter: {
            with_view_model(
                app->view_spectrum, WlanSpectrumViewModel * m,
                { m->filter = (WlanSpecFilter)((m->filter + 1) % 3); }, true);
            consumed = true;
            break;
        }
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeTick && s_running) {
        bool locked = false, paused = false;
        with_view_model(
            app->view_spectrum, WlanSpectrumViewModel * m,
            {
                locked = m->locked;
                paused = m->paused;
            },
            false);
        if(!paused) {
            // Close the dwell just finished for s_cur_ch.
            wlan_spectrum_set_dwell(
                &s_spec, s_cur_ch, (uint16_t)s_dwell_count, s_dwell_peak);
            if(!locked) {
                if(s_cur_ch >= SPEC_MAX_CH) {
                    wlan_spectrum_commit_sweep(&s_spec);
                    s_cur_ch = SPEC_MIN_CH;
                } else {
                    s_cur_ch++;
                }
            } else {
                // Locked: each tick is its own single-channel "sweep" row.
                wlan_spectrum_commit_sweep(&s_spec);
            }
            s_dwell_count = 0;
            s_dwell_peak = -128;
            wlan_hal_set_channel(s_cur_ch);
            with_view_model(
                app->view_spectrum, WlanSpectrumViewModel * m,
                { m->cur_channel = s_cur_ch; }, true);
        }
    }
    return consumed;
}

void wlan_app_scene_spectrum_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    s_running = false;
    s_dwell_count = 0;
    s_dwell_peak = -128;
}
