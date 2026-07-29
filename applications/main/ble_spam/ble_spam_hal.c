#include "ble_spam_hal.h"

#include <esp_bt.h>
#include <esp_nimble_hci.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/util/util.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <btshim.h>
#include <string.h>

/* Minimum free internal heap required to bring up the BLE controller + NimBLE
 * host for spam. The gate is kept from the Bluedroid build; NimBLE's host is
 * considerably lighter than Bluedroid (no GATT/SMP DB brought up for pure
 * advertising), so the *real* floor is lower than this — the integrator is
 * expected to retune BLE_SPAM_MIN_FREE_INTERNAL downward after measuring the
 * NimBLE steady-state cost. Until then we keep the (conservative) gate call so
 * a low-RAM boot still refuses gracefully instead of asserting inside the host.
 *
 * Historical Bluedroid note (still roughly informative): a running controller
 * consumes ~36 KB and the first host init costs a one-time chunk of persistent
 * globals that deinit does not reclaim. */
#define BLE_SPAM_MIN_FREE_INTERNAL (24 * 1024) /* NimBLE: ~half of Bluedroid */

#define TAG "BleSpamHal"

static volatile bool s_advertising = false;
static volatile bool s_host_synced = false;
/* Keep the stack up across attacks: start() is idempotent, the running scene
 * only stops *advertising* on exit, and full teardown happens once when leaving
 * the spam section. Cycling controller+host per attack is expensive and can
 * fragment/leak internal RAM. */
static bool s_hal_started = false;

/* own address type: we always advertise from a random static address (rotated
 * per packet by ble_spam_hal_set_random_addr) so popups don't dedupe. */
static uint8_t s_own_addr_type = BLE_OWN_ADDR_RANDOM;

/* NimBLE advertising params. Migration contract mandates non-connectable.
 * NOTE (behavior change vs Bluedroid): the Bluedroid build used ADV_TYPE_IND
 * (connectable undirected) so pairing-style popups could be tapped. Here
 * conn_mode = NON + disc_mode = NON yields ADV_NONCONN_IND (pure broadcast),
 * which is the fastest to cycle and is what most continuity/proximity spam
 * relies on. TODO: if Android Fast Pair (or any pairing popup) regresses,
 * switch conn_mode to BLE_GAP_CONN_MODE_UND and disc_mode to
 * BLE_GAP_DISC_MODE_GEN for that spam type. */
static struct ble_gap_adv_params s_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_NON,
    .disc_mode = BLE_GAP_DISC_MODE_NON,
    .itvl_min = 0x20, // 20ms (0.625ms units)
    .itvl_max = 0x40, // 40ms
    .channel_map = 0, // 0 => all three advertising channels
    .filter_policy = 0,
    .high_duty_cycle = 0,
};

/* Single collapsed GAP event callback (replaces the Bluedroid multi-event
 * handler). For non-connectable broadcast spam there is essentially nothing to
 * service — set-data/start/stop are all synchronous now — but NimBLE requires a
 * callback pointer, and BLE_GAP_EVENT_ADV_COMPLETE would fire if a finite
 * duration were ever used. */
static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    UNUSED(arg);
    switch(event->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        break;
    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host task + sync/reset callbacks
// ---------------------------------------------------------------------------

static void ble_spam_on_sync(void) {
    /* Controller and host are in sync; safe to set addresses and advertise. */
    s_host_synced = true;
}

static void ble_spam_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
    s_host_synced = false;
    s_advertising = false;
}

static void ble_spam_host_task(void* param) {
    UNUSED(param);
    ESP_LOGI(TAG, "NimBLE host task started");
    /* Returns only after nimble_port_stop(). */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_spam_hal_have_ram(void) {
    /* WiFi released the BLE controller's RAM this boot — it cannot be re-inited
     * until reboot. Report "no RAM" so the app gates off gracefully. */
    if(bt_is_mem_released()) return false;
    /* If the btshim serial stack is currently up (Bluetooth enabled in
     * Settings), it is holding RAM right now — but ble_spam_hal_start() stops it
     * first and reclaims that RAM before it needs it. So don't refuse at the
     * launch gate just because free looks low; there will be plenty once the
     * serial stack is swapped out. The strict free check only applies when BT is
     * already off (stack down, nothing to reclaim). */
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        return true;
    }
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= BLE_SPAM_MIN_FREE_INTERNAL;
}

bool ble_spam_hal_start(void) {
    // Already up from a previous attack in this session: reuse it. This is what
    // keeps repeated attacks from cycling (and leaking) the controller/host.
    if(s_hal_started) {
        ESP_LOGI(TAG, "BLE spam HAL already started, reusing");
        return true;
    }

    // WiFi released the BLE controller's RAM this boot — esp_bt_controller_init()
    // would fault. Refuse cleanly; the radio needs a reboot to come back.
    if(bt_is_mem_released()) {
        ESP_LOGE(TAG, "BLE controller RAM was released for WiFi; reboot to use BLE");
        return false;
    }

    ESP_LOGI(TAG, "Starting BLE spam HAL...");

    // Stop btshim BLE stack
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    // Memory gate (kept from Bluedroid build; NimBLE needs less — integrator to
    // retune). If bring-up fails mid-init it can assert inside ESP-IDF rather
    // than returning an error, so check now (after freeing the serial stack).
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if(free_internal < BLE_SPAM_MIN_FREE_INTERNAL) {
        ESP_LOGE(
            TAG,
            "Not enough RAM for BLE spam: %u free, need >= %u",
            (unsigned)free_internal,
            (unsigned)BLE_SPAM_MIN_FREE_INTERNAL);
        return false;
    }

    // Init BLE controller (UNCHANGED from Bluedroid build — controller stays)
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        return false;
    }

    // Init NimBLE host (replaces esp_bluedroid_init_with_cfg/enable)
    err = esp_nimble_init();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        return false;
    }

    // Host config: sync/reset callbacks. sync_cb fires once controller+host are
    // in sync — only then may we set a random address / start advertising.
    ble_hs_cfg.sync_cb = ble_spam_on_sync;
    ble_hs_cfg.reset_cb = ble_spam_on_reset;

    s_advertising = false;
    s_host_synced = false;

    // Spawn the NimBLE host task (runs nimble_port_run()).
    nimble_port_freertos_init(ble_spam_host_task);

    // Wait for host sync (up to ~1s). Without this the first set_rand_addr /
    // adv_start would fail with BLE_HS_ENOTSYNCED.
    for(int i = 0; i < 100 && !s_host_synced; i++) {
        furi_delay_ms(10);
    }
    if(!s_host_synced) {
        ESP_LOGE(TAG, "NimBLE host failed to sync");
        return false;
    }

    // Set TX power to maximum. esp_ble_tx_power_set is a CONTROLLER-level HCI
    // API (declared in esp_bt.h) and is independent of the host, so it survives
    // the Bluedroid->NimBLE swap unchanged.
#ifdef ESP_PWR_LVL_P21
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P21);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P21);
#else
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
#endif

    // Seed an initial random static address so the first advertise has a valid
    // own-address to use with BLE_OWN_ADDR_RANDOM.
    ble_spam_hal_set_random_addr();

    s_hal_started = true;
    ESP_LOGI(TAG, "BLE spam HAL ready");
    return true;
}

void ble_spam_hal_stop(void) {
    if(!s_hal_started) {
        return; // nothing to tear down (idempotent — safe from scene_main on_enter)
    }
    s_hal_started = false;
    ESP_LOGI(TAG, "Stopping BLE spam HAL...");

    // Stop advertising
    if(s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
        furi_delay_ms(50);
    }

    // Teardown NimBLE host: stop the port (unblocks nimble_port_run in the host
    // task, which then self-deinits) then free host resources.
    nimble_port_stop();
    nimble_port_deinit();
    s_host_synced = false;
    furi_delay_ms(50);

    // Teardown controller (UNCHANGED)
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    // Restart btshim
    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);

    ESP_LOGI(TAG, "BLE spam HAL stopped, btshim restored");
}

bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len) {
    if(len > 31) return false;
    if(!s_host_synced) return false;

    /* Rapid payload switch (contract path): stop -> set data -> start. NimBLE
     * requires advertising be stopped before adv data can be replaced, and all
     * three calls are synchronous (no async *_COMPLETE event round-trip like
     * Bluedroid), so the whole switch completes inline here. The caller
     * (spam_thread_fn) also calls ble_spam_hal_stop_adv() first, so the stop
     * below is normally a no-op; it stays for robustness. */
    if(s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
    }

    int rc = ble_gap_adv_set_data(data, len);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %d", rc);
        return false;
    }

    rc = ble_gap_adv_start(
        s_own_addr_type, NULL, BLE_HS_FOREVER, &s_adv_params, ble_gap_event, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return false;
    }
    s_advertising = true;
    return true;
}

void ble_spam_hal_stop_adv(void) {
    if(s_advertising) {
        /* Synchronous in NimBLE — returns once the controller has halted adv. */
        ble_gap_adv_stop();
        s_advertising = false;
    }
}

void ble_spam_hal_set_random_addr(void) {
    uint8_t addr[6];
    furi_hal_random_fill_buf(addr, 6);
    // Random static address: top two bits of MSB must be 11 (0xC0..0xFF).
    addr[5] = (addr[5] & 0x3F) | 0xC0;
    /* NimBLE ble_addr_t / HCI take the address little-endian (LSB first), same
     * byte order furi_hal_random filled, so the "static" high bits live in the
     * most-significant byte = addr[5]. */
    int rc = ble_hs_id_set_rnd(addr);
    if(rc != 0) {
        ESP_LOGW(TAG, "ble_hs_id_set_rnd: %d", rc);
    }
}

void ble_spam_hal_set_addr(const uint8_t addr[6]) {
    uint8_t a[6];
    memcpy(a, addr, 6);
    // Ensure valid random static format (MSB is addr[5], little-endian).
    a[5] = (a[5] & 0x3F) | 0xC0;
    int rc = ble_hs_id_set_rnd(a);
    if(rc != 0) {
        ESP_LOGW(TAG, "ble_hs_id_set_rnd (clone): %d", rc);
    }
}
