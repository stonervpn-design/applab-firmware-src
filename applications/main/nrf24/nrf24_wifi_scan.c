#include "nrf24_wifi_scan.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>

#define TAG "Nrf24WifiScan"

static bool s_netif_initialized = false;

/* Shared request block. Heap-allocated and reference-counted (refs starts at 2:
 * one for the caller, one for the task) so that on a scan timeout the caller can
 * walk away without freeing memory the still-running task will write to — the
 * last side to release frees everything. `abandoned` tells the task the caller is
 * gone so it must NOT touch the caller's out-pointers. */
typedef struct {
    wifi_ap_record_t** out_records;
    uint16_t* out_count;
    uint16_t max_count;
    bool result;
    SemaphoreHandle_t done;
    SemaphoreHandle_t lock;
    int refs;
    bool abandoned;
} ScanReq;

static void scanreq_release(ScanReq* req) {
    xSemaphoreTake(req->lock, portMAX_DELAY);
    int remaining = --req->refs;
    xSemaphoreGive(req->lock);
    if(remaining == 0) {
        vSemaphoreDelete(req->done);
        vSemaphoreDelete(req->lock);
        free(req);
    }
}

static bool wifi_init_once(void) {
    esp_err_t err = esp_netif_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(err));
        return false;
    }
    /* Only create the default STA netif if another app hasn't already done so;
     * esp_netif_create_default_wifi_sta() asserts on duplicate keys. */
    if(!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    s_netif_initialized = true;
    return true;
}

/* Runs as a real FreeRTOS task — esp_wifi_* requires a proper pthread/TLS
 * context that FuriThread wrappers do not provide. Results go into task-local
 * storage and are only published to the caller's pointers under the lock, iff
 * the caller is still waiting. */
static void wifi_scan_task(void* arg) {
    ScanReq* req = arg;
    wifi_ap_record_t* records = NULL;
    uint16_t out_count = 0;
    bool result = false;

    if(!wifi_init_once()) goto done;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 4;

    esp_err_t err = esp_wifi_init(&cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        goto done;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        goto done;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        esp_wifi_stop();
        esp_wifi_deinit();
        goto done;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if(count > req->max_count) count = req->max_count;

    if(count > 0) {
        records = malloc(count * sizeof(wifi_ap_record_t));
        if(records) {
            esp_wifi_scan_get_ap_records(&count, records);
            out_count = count;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    result = true;

done:
    xSemaphoreTake(req->lock, portMAX_DELAY);
    if(!req->abandoned && result) {
        *req->out_records = records; /* hand ownership to the caller */
        *req->out_count = out_count;
        records = NULL;
    }
    req->result = result;
    xSemaphoreGive(req->lock);

    free(records); /* frees the buffer if the caller abandoned us or the scan failed */
    xSemaphoreGive(req->done);
    scanreq_release(req);
    vTaskDelete(NULL);
}

bool nrf24_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    *out_records = NULL;
    *out_count = 0;

    ScanReq* req = calloc(1, sizeof(ScanReq));
    if(!req) return false;
    req->out_records = out_records;
    req->out_count = out_count;
    req->max_count = max_count;
    req->result = false;
    req->refs = 2; /* caller + task */
    req->done = xSemaphoreCreateBinary();
    req->lock = xSemaphoreCreateMutex();
    if(!req->done || !req->lock) {
        if(req->done) vSemaphoreDelete(req->done);
        if(req->lock) vSemaphoreDelete(req->lock);
        free(req);
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        wifi_scan_task, "Nrf24WifiScan", 6 * 1024, req, 5, NULL, 0);
    if(ok != pdPASS) {
        /* Task never started, so it holds no reference — free directly. */
        vSemaphoreDelete(req->done);
        vSemaphoreDelete(req->lock);
        free(req);
        return false;
    }

    /* Scan typically takes ~3 s; cap at 8 s. On timeout we mark the request
     * abandoned instead of tearing down memory the task may still use. */
    bool result;
    if(xSemaphoreTake(req->done, pdMS_TO_TICKS(8000)) == pdTRUE) {
        result = req->result;
    } else {
        xSemaphoreTake(req->lock, portMAX_DELAY);
        req->abandoned = true;
        xSemaphoreGive(req->lock);
        result = false;
    }

    scanreq_release(req);
    return result;
}
