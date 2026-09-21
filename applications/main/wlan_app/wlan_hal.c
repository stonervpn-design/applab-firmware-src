#include "wlan_hal.h"
#include "wlan_probe_parse.h"

#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>
/* esp_bt_main.h (Bluedroid host) removed — only esp_bt_controller_* (esp_bt.h)
 * is used here, and the host is NimBLE now. */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <furi.h>
#include <btshim.h>
#include <string.h>
#include <stdlib.h>

#define TAG "WlanHal"
// Worker macht esp_wifi/lwIP-Calls; 4096 Words (16 KB) statt 8192 (32 KB),
// um internes RAM für esp_wifi_init zu sparen. Bei Stack-Overflow erhöhen.
#define WLAN_HAL_WORKER_STACK 4096

typedef enum {
    WCMD_INIT_START,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_SEND_ETH_RAW,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_RUN_FN,
    WCMD_QUIT,
} WlanCmdType;

typedef struct {
    WlanCmdType type;
    union {
        struct {
            wifi_scan_config_t* config;
            wifi_ap_record_t** out_records;
            uint16_t* out_count;
            uint16_t max_count;
        } scan;
        struct {
            char ssid[33];
            char password[65];
            uint8_t bssid[6];
            uint8_t channel;
            bool bssid_set;
        } connect;
        struct {
            uint8_t* buf;   // heap-alloziert vom Sender, free durch Consumer
            uint16_t len;
        } send_eth;
        struct {
            uint8_t channel;
        } set_channel;
        struct {
            bool enable;
            wifi_promiscuous_cb_t cb;
        } set_promisc;
        struct {
            uint8_t buf[128];
            uint16_t len;
        } send_raw;
        struct {
            WlanHalWorkerFn fn;
            void* arg;
        } run_fn;
    };
    volatile bool* done;
    volatile bool* result;
} WlanCmd;

static bool s_started = false;
static bool s_bt_was_on = false;
static bool s_netif_inited = false;
static bool s_event_loop_created = false;
/* Injection-only features (beacon spam, karma, deauth) set this before
 * wlan_hal_start() so WCMD_INIT_START skips esp_netif/lwIP setup. On this
 * no-PSRAM board that IP stack costs ~9.8 KB of DMA-capable RAM, which
 * otherwise leaves esp_wifi_80211_tx with 0 bytes for a TX buffer (NO_MEM).
 * Consumed (reset) inside WCMD_INIT_START. */
static volatile bool s_next_init_skip_netif = false;
static esp_netif_t* s_netif_sta = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static bool s_event_handlers_registered = false;
static volatile uint32_t s_own_ip = 0;
static volatile uint32_t s_own_netmask = 0;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_buf;

static void wlan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_own_ip = 0;
        s_own_netmask = 0;
        if(s_wifi_auto_reconnect) {
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_own_netmask = event->ip_info.netmask.addr;
        s_wifi_connected = true;
    }
}

static void wlan_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    WlanCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_START:
            // The default event loop is needed for esp_event_handler_register
            // below and for esp_wifi's own events; it is cheap, always create it.
            if(!s_event_loop_created) {
                esp_event_loop_create_default();
                s_event_loop_created = true;
            }
            // esp_netif/lwIP (the STA IP interface) is only needed to obtain an
            // IP via DHCP (connect / evil-portal bridge). Injection and scan do
            // not need it, and on this no-PSRAM board it costs ~9.8 KB of DMA
            // RAM that the 802.11 TX buffer needs. Skip it when hinted.
            if(!s_netif_inited && !s_next_init_skip_netif) {
                esp_netif_init();
                s_netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if(!s_netif_sta) {
                    s_netif_sta = esp_netif_create_default_wifi_sta();
                }
                s_netif_inited = true;
            }
            s_next_init_skip_netif = false; // consume the one-shot hint
            if(!s_event_handlers_registered) {
                esp_event_handler_register(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &wlan_event_handler, NULL);
                esp_event_handler_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &wlan_event_handler, NULL);
                s_event_handlers_registered = true;
            }

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            cfg.static_rx_buf_num = 2;
            cfg.dynamic_rx_buf_num = 4;
            cfg.dynamic_tx_buf_num = 8;

            err = esp_wifi_init(&cfg);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                ok = false;
            } else {
                // Push TX power to the chip ceiling (84 = 21 dBm) for maximum range.
                esp_wifi_set_max_tx_power(84);
            }
            break;

        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            break;

        case WCMD_CONNECT: {
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            vTaskDelay(pdMS_TO_TICKS(100));

            wifi_config_t wcfg = {0};
            strncpy((char*)wcfg.sta.ssid, cmd.connect.ssid, 32);
            if(cmd.connect.password[0]) {
                strncpy((char*)wcfg.sta.password, cmd.connect.password, 64);
                wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
            } else {
                wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            if(cmd.connect.bssid_set) {
                wcfg.sta.bssid_set = true;
                memcpy(wcfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wcfg.sta.channel = cmd.connect.channel;
            }
            wcfg.sta.pmf_cfg.capable = false;
            wcfg.sta.pmf_cfg.required = false;
            s_wifi_auto_reconnect = true;
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
                ok = false;
            }
            break;
        }

        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            break;

        case WCMD_SEND_ETH_RAW: {
            int eth_err = esp_wifi_internal_tx(WIFI_IF_STA, cmd.send_eth.buf, cmd.send_eth.len);
            if(eth_err != 0) {
                static uint32_t eth_err_count = 0;
                static uint32_t eth_err_last_log = 0;
                eth_err_count++;
                if(eth_err_count - eth_err_last_log >= 20) {
                    eth_err_last_log = eth_err_count;
                    ESP_LOGW(TAG, "internal_tx err=%d (count=%lu)",
                        eth_err, (unsigned long)eth_err_count);
                }
            }
            free(cmd.send_eth.buf);
            break;
        }

        case WCMD_SET_CHANNEL:
            esp_wifi_set_channel(cmd.set_channel.channel, WIFI_SECOND_CHAN_NONE);
            break;

        case WCMD_SET_PROMISC:
            // CB immer setzen (auch auf NULL), sonst bleibt ein zuvor
            // installierter RX-Callback aus einem anderen Subsystem aktiv.
            esp_wifi_set_promiscuous_rx_cb(cmd.set_promisc.enable ? cmd.set_promisc.cb : NULL);
            esp_wifi_set_promiscuous(cmd.set_promisc.enable);
            break;

        case WCMD_SEND_RAW: {
            // en_sys_seq=true: System füllt die Sequence-Number selbst → keine
            // duplizierten Frames mit fixem Seq aus dem Template.
            esp_err_t tx_err = esp_wifi_80211_tx(
                WIFI_IF_STA, cmd.send_raw.buf, cmd.send_raw.len, true);
            if(tx_err != ESP_OK) {
                ESP_LOGD(TAG, "80211_tx: %s", esp_err_to_name(tx_err));
            }
            break;
        }

        case WCMD_SCAN: {
            err = esp_wifi_scan_start(cmd.scan.config, true);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
                *cmd.scan.out_count = 0;
                *cmd.scan.out_records = NULL;
                ok = false;
                break;
            }
            uint16_t count = 0;
            esp_wifi_scan_get_ap_num(&count);
            /* TEMP DIAGNOSTIC: how many APs did the radio actually see? */
            ESP_LOGW(TAG, "scan done: ap_num=%u", count);
            if(count > cmd.scan.max_count) count = cmd.scan.max_count;
            if(count > 0) {
                *cmd.scan.out_records = malloc(count * sizeof(wifi_ap_record_t));
                if(*cmd.scan.out_records) {
                    esp_wifi_scan_get_ap_records(&count, *cmd.scan.out_records);
                } else {
                    count = 0;
                }
            } else {
                *cmd.scan.out_records = NULL;
            }
            *cmd.scan.out_count = count;
            break;
        }

        case WCMD_RUN_FN:
            if(cmd.run_fn.fn) cmd.run_fn.fn(cmd.run_fn.arg);
            break;

        case WCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.done) *cmd.done = true;
            vTaskDelete(NULL);
            return;
        }

        if(cmd.result) *cmd.result = ok;
        if(cmd.done) *cmd.done = true;
    }
}

static bool wlan_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(4, sizeof(WlanCmd));
    if(!s_cmd_queue) return false;

    s_worker_stack = heap_caps_malloc(
        WLAN_HAL_WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_worker_stack) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Cannot alloc worker stack");
        return false;
    }

    s_worker_task = xTaskCreateStaticPinnedToCore(
        wlan_worker_fn, "WlanWorker", WLAN_HAL_WORKER_STACK,
        NULL, 5, s_worker_stack, &s_worker_buf, 0);
    return s_worker_task != NULL;
}

bool wlan_hal_ensure_worker(void) {
    return wlan_ensure_worker();
}

static void wlan_send_cmd_sync(WlanCmd* cmd) {
    volatile bool done = false;
    cmd->done = &done;
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    while(!done) {
        furi_delay_ms(10);
    }
}

/* Fully release the BLE controller + Bluedroid so WiFi gets all their RAM back.
 * On this no-PSRAM board the BT stack holds ~64 KB and its plain deinit leaves a
 * ~13 KB residual that starves esp_wifi_init ("search fails after using
 * Bluetooth"). We stop the btshim serial stack, then deinit + mem_release the
 * controller. NOTE: after this BT cannot re-init until reboot — that is the
 * intended "shut Bluetooth down fully for WiFi" behaviour; the two radios are
 * mutually exclusive on this board. */
void wlan_hal_release_bt(void) {
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt); // idempotent: no-op if the serial stack was never up
    furi_record_close(RECORD_BT);
    furi_delay_ms(50);

    // Also covers a BLE app (Spam/Walk) that borrowed the controller directly.
    esp_bt_controller_status_t st = esp_bt_controller_get_status();
    if(st == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
        furi_delay_ms(20);
        st = esp_bt_controller_get_status();
    }
    if(st == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_bt_controller_deinit();
        furi_delay_ms(20);
    }
    // Reclaim the controller's reserved RAM. Safe to call repeatedly.
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    // Latch it: esp_bt_controller_init() now faults, so BLE apps must refuse to
    // start (not crash) until the next reboot.
    bt_mark_mem_released();
}

bool wlan_hal_start(void) {
    if(s_started) return true;

    wlan_hal_release_bt();
    s_bt_was_on = false; // BT is fully released now; do not try to restore it

    if(!wlan_ensure_worker()) return false;

    volatile bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_START, .result = &result};
    wlan_send_cmd_sync(&cmd);

    if(result) {
        s_started = true;
        ESP_LOGI(TAG, "WiFi started");
    }
    return result;
}

void wlan_hal_stop(void) {
    if(s_started) {
        WlanCmd cmd = {.type = WCMD_STOP_DEINIT};
        wlan_send_cmd_sync(&cmd);
        s_started = false;
        ESP_LOGI(TAG, "WiFi stopped");
    }

    if(s_bt_was_on) {
        Bt* bt = furi_record_open(RECORD_BT);
        bt_start_stack(bt);
        furi_record_close(RECORD_BT);
        s_bt_was_on = false;
    }
}

bool wlan_hal_is_started(void) {
    return s_started;
}

void wlan_hal_hint_skip_netif(void) {
    // One-shot hint honoured by the next wlan_hal_start() that actually inits
    // WiFi. TX-only features call this to keep the ~9.8 KB IP-stack DMA free.
    if(!s_started) s_next_init_skip_netif = true;
}

bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel) {
    if(!ssid) return false;

    // Connect needs the STA netif for DHCP/IP. If WiFi is already running in
    // injection (netif-less) mode, restart it so the netif is created.
    if(s_started && !s_netif_sta) {
        wlan_hal_stop();
    }
    if(!s_started) {
        s_next_init_skip_netif = false; // connect requires the IP stack
        if(!wlan_hal_start()) return false;
    }

    WlanCmd cmd = {.type = WCMD_CONNECT};
    memset(&cmd.connect, 0, sizeof(cmd.connect));
    strncpy(cmd.connect.ssid, ssid, 32);
    if(password && password[0]) {
        strncpy(cmd.connect.password, password, 64);
    }
    if(bssid) {
        memcpy(cmd.connect.bssid, bssid, 6);
        cmd.connect.bssid_set = true;
    }
    cmd.connect.channel = channel;

    volatile bool result = false;
    cmd.result = &result;
    wlan_send_cmd_sync(&cmd);
    return result;
}

void wlan_hal_disconnect(void) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_DISCONNECT};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_is_connected(void) {
    return s_wifi_connected;
}

uint32_t wlan_hal_get_own_ip(void) {
    return s_own_ip;
}

uint32_t wlan_hal_get_netmask(void) {
    return s_own_netmask;
}

bool wlan_hal_get_own_mac(uint8_t out[6]) {
    if(!s_started) return false;
    return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
}

void wlan_hal_random_mac(uint8_t out[6]) {
    for(int i = 0; i < 6; i++) out[i] = rand() & 0xFF;
    out[0] = (out[0] & 0xFE) | 0x02; /* unicast + locally administered */
}

uint32_t wlan_hal_get_gw_ip(void) {
    if(!s_netif_sta) return 0;
    esp_netif_ip_info_t info;
    if(esp_netif_get_ip_info(s_netif_sta, &info) != ESP_OK) return 0;
    return info.gw.addr;
}

bool wlan_hal_send_eth_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue) {
        ESP_LOGW(TAG, "send_eth_raw: not started (s=%d q=%p)", s_started, s_cmd_queue);
        return false;
    }
    if(!data || len < 14 || len > 1600) return false;
    uint8_t* buf = malloc(len);
    if(!buf) {
        ESP_LOGE(TAG, "send_eth_raw: malloc(%u) failed", (unsigned)len);
        return false;
    }
    memcpy(buf, data, len);
    WlanCmd cmd = {.type = WCMD_SEND_ETH_RAW, .done = NULL, .result = NULL};
    cmd.send_eth.buf = buf;
    cmd.send_eth.len = len;
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        static uint32_t qfull_count = 0;
        static uint32_t qfull_last_log = 0;
        qfull_count++;
        if(qfull_count - qfull_last_log >= 20) {
            qfull_last_log = qfull_count;
            ESP_LOGW(TAG, "send_eth_raw: cmd queue full (count=%lu)",
                (unsigned long)qfull_count);
        }
        free(buf);
        return false;
    }
    return true;
}

void wlan_hal_set_channel(uint8_t channel) {
    if(!s_started || channel < 1 || channel > 14) return;
    WlanCmd cmd = {.type = WCMD_SET_CHANNEL, .set_channel = {.channel = channel}};
    wlan_send_cmd_sync(&cmd);
}

void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_SET_PROMISC, .set_promisc = {.enable = enable, .cb = cb}};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || len > 128) return false;
    WlanCmd cmd = {.type = WCMD_SEND_RAW, .done = NULL, .result = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg) {
    if(!fn) return false;
    // Lazy-init the worker queue + task. Evil Portal is entered directly from
    // the menu without going through wlan_hal_start (no STA scan), so the
    // worker may not exist yet. Safe to call repeatedly; no-ops if already up.
    if(!wlan_ensure_worker()) return false;
    WlanCmd cmd = {.type = WCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    wlan_send_cmd_sync(&cmd);
    return true;
}

void wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    if(!s_started) {
        if(out_records) *out_records = NULL;
        *out_count = 0;
        return;
    }
    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        // Longer per-channel dwell so weaker/more distant APs are heard.
        .scan_time = {.active = {.min = 120, .max = 300}},
    };
    WlanCmd cmd = {
        .type = WCMD_SCAN,
        .scan = {
            .config = &scan_config,
            .out_records = out_records,
            .out_count = out_count,
            .max_count = max_count,
        },
    };
    wlan_send_cmd_sync(&cmd);
}

// ---------------------------------------------------------------------------
// Beacon-Spam: dedizierter xTaskCreate-Task (parallel zum Worker), nutzt
// esp_wifi_80211_tx() direkt. Frame-Counter + Stop-Flag sind volatile.
// ---------------------------------------------------------------------------

static const uint8_t beacon_packet_template[] = {
    0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00,
    0x00, 0x00, 0xe8, 0x03, 0x31, 0x00, 0x00, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x08,
    0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01,
    0x01, 0x30, 0x18, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x02,
    0x00, 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04, 0x01,
    0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
};

static const char* beacon_funny_ssids[] = {
    "Mom Use This One", "Abraham Linksys", "Benjamin FrankLAN",
    "Martin Router King", "John Wilkes Bluetooth", "Pretty Fly for a Wi-Fi",
    "Bill Wi the Science Fi", "I Believe Wi Can Fi", "Tell My Wi-Fi Love Her",
    "No More Mister Wi-Fi", "LAN Solo", "The LAN Before Time",
    "Silence of the LANs", "House LANister", "Winternet Is Coming",
    "FBI Surveillance Van 4", "Area 51 Test Site", "Never Gonna Give You Up",
    "Loading...", "VIRUS.EXE", "Free Public Wi-Fi", "404 Wi-Fi Unavailable",
    NULL,
};

static const char* beacon_rickroll_ssids[] = {
    "01 Never gonna give you up",
    "02 Never gonna let you down",
    "03 Never gonna run around",
    "04 And desert you",
    "05 Never gonna make you cry",
    "06 Never gonna say goodbye",
    "07 Never gonna tell a lie",
    "08 And hurt you",
    NULL,
};

static volatile bool s_beacon_active = false;
static volatile uint32_t s_beacon_frames = 0;
static volatile int s_beacon_last_tx = ESP_OK; // last esp_wifi_80211_tx() result
static TaskHandle_t s_beacon_task = NULL;
static WlanHalBeaconMode s_beacon_mode = WlanHalBeaconModeFunny;
static char s_beacon_base_ssid[33] = {0};

static void prepare_beacon_packet(uint8_t* packet, const uint8_t* mac,
                                  const char* ssid, uint8_t channel) {
    memcpy(packet, beacon_packet_template, sizeof(beacon_packet_template));
    memcpy(&packet[10], mac, 6);
    memcpy(&packet[16], mac, 6);
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    if(ssid_len > 32) ssid_len = 32;
    packet[37] = ssid_len;
    memcpy(&packet[38], ssid, ssid_len);
    packet[82] = channel;
}

static void beacon_spam_task(void* param) {
    (void)param;
    const char** ssid_list = NULL;
    char gen_ssid[64];
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    int ssid_index = 0;
    int counter = 1;
    uint8_t channel = 1;

    srand((unsigned)esp_log_timestamp());

    // Promiscuous (cb=NULL) erlaubt 80211_tx auf STA-Interface.
    esp_wifi_set_promiscuous(true);

    while(s_beacon_active) {
        const char* current_ssid = NULL;
        switch(s_beacon_mode) {
        case WlanHalBeaconModeFunny:
            ssid_list = beacon_funny_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRickroll:
            ssid_list = beacon_rickroll_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRandom:
            snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            current_ssid = gen_ssid;
            break;
        case WlanHalBeaconModeCustom:
            if(s_beacon_base_ssid[0]) {
                snprintf(gen_ssid, sizeof(gen_ssid), "%s%d",
                    s_beacon_base_ssid, counter++);
                if(counter > 9999) counter = 1;
            } else {
                snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            }
            current_ssid = gen_ssid;
            break;
        }
        if(!current_ssid) current_ssid = "Flipper";

        // Random Locally-Administered Unicast MAC.
        wlan_hal_random_mac(mac);

        prepare_beacon_packet(packet, mac, current_ssid, channel);
        // en_sys_seq MUST be true here: after esp_wifi_start() in STA mode the
        // connection context is "set up", so IDF 5.4 returns ESP_ERR_INVALID_ARG
        // for en_sys_seq==false — which pinned the frame counter at 0. The
        // working deauth path uses true for the same reason.
        s_beacon_last_tx = esp_wifi_80211_tx(WIFI_IF_STA, packet,
               sizeof(beacon_packet_template), true);
        if(s_beacon_last_tx == ESP_OK) {
            s_beacon_frames++;
        }

        if((s_beacon_frames % 5) == 0) {
            channel++;
            if(channel > 11) channel = 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_wifi_set_promiscuous(false);
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_beacon_spam_start(WlanHalBeaconMode mode, const char* base_ssid) {
    if(s_beacon_active || s_beacon_task) return;
    if(!s_started) {
        s_next_init_skip_netif = true; // TX-only: skip IP stack to free DMA RAM
        if(!wlan_hal_start()) return;
    }
    if(s_wifi_connected) wlan_hal_disconnect();

    s_beacon_mode = mode;
    memset(s_beacon_base_ssid, 0, sizeof(s_beacon_base_ssid));
    if(mode == WlanHalBeaconModeCustom && base_ssid) {
        strncpy(s_beacon_base_ssid, base_ssid, sizeof(s_beacon_base_ssid) - 1);
    }
    s_beacon_frames = 0;
    s_beacon_active = true;
    BaseType_t rc = xTaskCreate(beacon_spam_task, "BeaconSpam",
        4096, NULL, 5, &s_beacon_task);
    if(rc != pdPASS) {
        s_beacon_active = false;
        s_beacon_task = NULL;
        ESP_LOGE(TAG, "beacon_spam: xTaskCreate failed");
    }
}

void wlan_hal_beacon_spam_stop(void) {
    if(!s_beacon_active && !s_beacon_task) return;
    s_beacon_active = false;
    // Task beendet sich selbst (vTaskDelete im Task). Auf Beendigung warten.
    while(s_beacon_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_beacon_spam_is_running(void) {
    return s_beacon_active;
}

uint32_t wlan_hal_beacon_spam_get_frame_count(void) {
    return s_beacon_frames;
}

const char* wlan_hal_beacon_spam_last_result(void) {
    return esp_err_to_name((esp_err_t)s_beacon_last_tx);
}

// ---- Karma -----------------------------------------------------------------
static volatile bool s_karma_active = false;
static TaskHandle_t s_karma_task = NULL;
static WlanSsidSet s_karma_pnl;          // harvested SSIDs (bounded 32)
static FuriMutex* s_karma_lock = NULL;
static volatile uint32_t s_karma_beacons = 0;

// Promiscuous RX callback: harvest probe-request SSIDs into the PNL set.
static void karma_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t mac[6];
    if(!wlan_probe_extract(pkt->payload, len, ssid, mac)) return;
    if(ssid[0] == '\0') return; // skip wildcard probes
    // SSID must fit the 802.11 beacon we rebroadcast (<=32) — already guaranteed.
    if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
        wlan_ssid_set_insert(&s_karma_pnl, ssid);
        furi_mutex_release(s_karma_lock);
    }
}

static void karma_task(void* param) {
    (void)param;
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    uint8_t channel = 1;
    uint16_t idx = 0;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(karma_promisc_cb);

    while(s_karma_active) {
        // Snapshot one SSID from the harvested PNL to advertise this iteration.
        char ssid[WLAN_PROBE_SSID_MAX] = {0};
        uint8_t count = 0;
        if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
            count = s_karma_pnl.count;
            if(count) {
                if(idx >= count) idx = 0;
                strncpy(ssid, s_karma_pnl.ssids[idx], WLAN_PROBE_SSID_MAX - 1);
                idx++;
            }
            furi_mutex_release(s_karma_lock);
        }
        if(ssid[0]) {
            wlan_hal_random_mac(mac);
            prepare_beacon_packet(packet, mac, ssid, channel);
            // en_sys_seq must be true (see beacon_spam_task note) — false yields
            // ESP_ERR_INVALID_ARG in STA mode on IDF 5.4.
            if(esp_wifi_80211_tx(WIFI_IF_STA, packet,
                   sizeof(beacon_packet_template), true) == ESP_OK) {
                s_karma_beacons++;
            }
            if((s_karma_beacons % 5) == 0) {
                channel++;
                if(channel > 11) channel = 1;
                esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous(false);
    s_karma_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_karma_start(void) {
    if(s_karma_active || s_karma_task) return;
    if(!s_started) {
        s_next_init_skip_netif = true; // TX-only: skip IP stack to free DMA RAM
        if(!wlan_hal_start()) return;
    }
    if(s_wifi_connected) wlan_hal_disconnect();
    if(!s_karma_lock) s_karma_lock = furi_mutex_alloc(FuriMutexTypeNormal);
    wlan_ssid_set_reset(&s_karma_pnl);
    s_karma_beacons = 0;
    s_karma_active = true;
    if(xTaskCreate(karma_task, "Karma", 4096, NULL, 5, &s_karma_task) != pdPASS) {
        s_karma_active = false;
        s_karma_task = NULL;
        ESP_LOGE(TAG, "karma: xTaskCreate failed");
    }
}

void wlan_hal_karma_stop(void) {
    if(!s_karma_active && !s_karma_task) return;
    s_karma_active = false;
    while(s_karma_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_karma_is_running(void) {
    return s_karma_active;
}

void wlan_hal_karma_get_stats(WlanHalKarmaStats* out) {
    out->beacons_sent = s_karma_beacons;
    out->harvested = 0;
    out->top_ssid[0] = '\0';
    if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
        out->harvested = s_karma_pnl.count;
        if(s_karma_pnl.count) {
            strncpy(out->top_ssid, s_karma_pnl.ssids[0], sizeof(out->top_ssid) - 1);
            out->top_ssid[sizeof(out->top_ssid) - 1] = '\0';
        }
        furi_mutex_release(s_karma_lock);
    }
}
