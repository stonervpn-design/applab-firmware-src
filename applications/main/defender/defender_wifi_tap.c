#include "defender_wifi_tap.h"

#include "../wlan_app/wlan_hal.h"

#include <esp_wifi.h>
#include <furi.h>
#include <string.h>

/* Ring buffer configuration: stores up to 48 deauth/disassoc records */
#define DEF_DEAUTH_RING 48
#define DEF_BEACON_RING 64

/* Volatile ring state to be safely accessed from WiFi task callback */
static volatile uint32_t s_write_idx = 0;
static volatile uint32_t s_read_idx = 0;
static DefTapDeauthRec s_ring[DEF_DEAUTH_RING];

/* Beacon ring, filled by the same promiscuous callback */
static volatile uint32_t s_bwrite_idx = 0;
static volatile uint32_t s_bread_idx = 0;
// On-demand (was ~3.1 KB always-resident BSS). Written by def_tap_cb on the
// WiFi task; allocated in _start before promiscuous is enabled, freed in _stop
// after it is disabled.
static DefenderBeaconRec* s_bring;
static volatile uint8_t s_tap_channel = 1;

/**
 * Promiscuous callback: runs in WiFi task context.
 * Filters for management frames, classifies them, and pushes deauth/disassoc
 * records to the ring. Implements drop-on-full behavior.
 */
static void def_tap_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(!s_bring) return; /* buffer freed / not yet allocated */
    /* Only interested in management frames */
    if(type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    /* Classify the frame */
    DefWifiFrame frame;
    if(!defender_wifi_classify(payload, len, &frame)) {
        return;  /* Too short, ignore */
    }

    /* Beacons feed the rogue-AP detector (evil-twin / beacon-flood). */
    if(frame.kind == DefWifiBeacon) {
        uint32_t bnext = (s_bwrite_idx + 1) % DEF_BEACON_RING;
        if(bnext == s_bread_idx) return; /* ring full, drop */
        DefenderBeaconRec* rec = &s_bring[s_bwrite_idx];
        memset(rec, 0, sizeof(*rec));
        memcpy(rec->bssid, frame.addr3, 6);
        rec->rssi = pkt->rx_ctrl.rssi;
        rec->channel = s_tap_channel;
        rec->ts_ms = 0; /* stamped on drain */
        /* SSID is the first tagged IE in a beacon: fixed params occupy bytes
           24..35, so tag id at [36], length at [37], SSID bytes at [38..]. */
        if(len >= 38 && payload[36] == 0x00) {
            uint8_t sl = payload[37];
            if(sl > ROGUE_SSID_MAX - 1) sl = ROGUE_SSID_MAX - 1;
            if((uint16_t)(38 + sl) <= len) {
                memcpy(rec->ssid, &payload[38], sl);
                rec->ssid[sl] = '\0';
            }
        }
        s_bwrite_idx = bnext;
        return;
    }

    /* Only capture deauth and disassoc frames beyond this point */
    if(frame.kind != DefWifiDeauth && frame.kind != DefWifiDisassoc) {
        return;
    }

    /* Calculate next write index */
    uint32_t next = (s_write_idx + 1) % DEF_DEAUTH_RING;

    /* Drop if ring is full */
    if(next == s_read_idx) {
        return;
    }

    /* Push record to ring (without timestamp; will be stamped on drain) */
    memcpy(s_ring[s_write_idx].bssid, frame.addr3, 6);
    s_ring[s_write_idx].broadcast = frame.broadcast_addr1;
    s_ring[s_write_idx].reason = frame.reason_code;
    s_ring[s_write_idx].rssi = pkt->rx_ctrl.rssi;
    s_ring[s_write_idx].ts_ms = 0;  /* Stamped on drain */

    s_write_idx = next;
}

/* Runs on the wlan worker task. esp_wifi_set_promiscuous_filter() calls
 * pthread_getspecific internally, which dereferences garbage when invoked from
 * a FuriThread (Flipper's FuriThread hijacks TLS slot 0) → LoadProhibited. The
 * worker is a plain FreeRTOS task with clean TLS, so run it there. */
static void def_set_promisc_filter_worker(void* arg) {
    (void)arg;
    esp_wifi_set_promiscuous_filter(
        &(wifi_promiscuous_filter_t){.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT});
}

/**
 * Start the WiFi promiscuous tap.
 * Releases Bluetooth, starts WiFi stack, sets channel, and enables
 * management frame capture.
 */
void defender_wifi_tap_start(uint8_t channel) {
    /* Release Bluetooth to reclaim ~64 KB of RAM for WiFi on no-PSRAM boards */
    wlan_hal_release_bt();

    /* Start WiFi if not already running */
    if(!wlan_hal_is_started()) {
        wlan_hal_start();
    }

    /* Disconnect if currently connected */
    if(wlan_hal_is_connected()) {
        wlan_hal_disconnect();
    }

    /* Set the WiFi channel */
    wlan_hal_set_channel(channel);

    /* Set promiscuous filter to management frames only — on the worker task,
     * NOT here on the GUI thread (see def_set_promisc_filter_worker). */
    wlan_hal_run_in_worker(def_set_promisc_filter_worker, NULL);

    /* Reset ring indices BEFORE enabling promiscuous mode so the RX callback
     * can never observe stale head/tail (cursor desync / lost startup frames) */
    s_write_idx = 0;
    s_read_idx = 0;
    s_bwrite_idx = 0;
    s_bread_idx = 0;

    /* Allocate the beacon ring before enabling the callback that fills it. */
    if(!s_bring) {
        s_bring = calloc(DEF_BEACON_RING, sizeof(*s_bring));
        if(!s_bring) return; /* no RAM — leave promiscuous off */
    }

    /* Enable promiscuous mode with our callback */
    wlan_hal_set_promiscuous(true, def_tap_cb);

    s_tap_channel = channel;
}

/**
 * Stop the WiFi promiscuous tap.
 */
void defender_wifi_tap_stop(void) {
    wlan_hal_set_promiscuous(false, NULL); /* unregister def_tap_cb (synchronous) */
    wlan_hal_stop();
    free(s_bring);
    s_bring = NULL;
}

/**
 * Change WiFi channel while tap is active.
 */
void defender_wifi_tap_set_channel(uint8_t ch) {
    wlan_hal_set_channel(ch);
    s_tap_channel = ch;
}

uint32_t defender_wifi_tap_drain_beacon(DefenderBeaconRec* out, uint32_t max) {
    if(!out || max == 0 || !s_bring) return 0;
    uint32_t count = 0;
    uint32_t tick = furi_get_tick();
    while(s_bread_idx != s_bwrite_idx && count < max) {
        memcpy(&out[count], &s_bring[s_bread_idx], sizeof(DefenderBeaconRec));
        out[count].ts_ms = tick;
        s_bread_idx = (s_bread_idx + 1) % DEF_BEACON_RING;
        count++;
    }
    return count;
}

/**
 * Drain deauth/disassoc records from the ring buffer.
 * Called from the main app loop (not ISR).
 * Timestamps each record with the current time on drain.
 *
 * @param out  Output buffer for records
 * @param max  Maximum records to return
 * @return     Number of records written to out
 */
uint32_t defender_wifi_tap_drain_deauth(DefTapDeauthRec* out, uint32_t max) {
    if(!out || max == 0) return 0;

    uint32_t count = 0;
    uint32_t current_tick = furi_get_tick();

    /* Pop records from the ring until empty or buffer full */
    while(s_read_idx != s_write_idx && count < max) {
        /* Copy record from ring */
        memcpy(&out[count], &s_ring[s_read_idx], sizeof(DefTapDeauthRec));

        /* Stamp timestamp on drain (to keep callback ISR-light) */
        out[count].ts_ms = current_tick;

        /* Advance read pointer */
        s_read_idx = (s_read_idx + 1) % DEF_DEAUTH_RING;
        count++;
    }

    return count;
}
