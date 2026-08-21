#include "furi_hal_rtc.h"

#include <time.h>

#include <nvs_flash.h>
#include <nvs.h>

/* Anti-theft state (PIN + fail counter) persistence.
 *
 * On the real Flipper these live in the STM32's battery-backed RTC backup
 * registers, which survive power-off. This ESP32 port keeps FuriHalRtcState in
 * plain RAM, so without backing the anti-theft PIN was lost on every reboot:
 * desktop_pin_code_is_set() ((pin_value >> 28) != 0) returned false after a
 * power-cycle, the lock-on-boot check never fired, and the device booted
 * UNLOCKED with the serial CLI re-enabled -- defeating the whole anti-theft
 * feature with a zero-tool power cycle (and the wipe-after-N-PINs never armed).
 * NVS survives reboot/power-cycle and is initialised (nvs_flash_init in
 * furi_hal.c) BEFORE furi_hal_rtc_init(), so we persist pin_value/pin_fails
 * there. Same rationale and mechanism as the BLE-Stealth / device-name settings
 * in furi_hal_version.c. Cleared on factory reset (reset_registers below).
 */
#define RTC_NVS_NS        "antitheft"
#define RTC_NVS_KEY_PIN   "pin_value"
#define RTC_NVS_KEY_FAILS "pin_fails"

static void furi_hal_rtc_nvs_store(const char* key, uint32_t value) {
    nvs_handle_t h;
    if(nvs_open(RTC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

typedef struct {
    FuriHalRtcHeapTrackMode heap_track_mode;
    FuriHalRtcBootMode boot_mode;
    FuriHalRtcLogDevice log_device;
    FuriHalRtcLogBaudRate log_baud_rate;
    uint8_t log_level;
    uint32_t flags;
    uint32_t fault_data;
    uint32_t pin_fails;
    uint32_t pin_value;
    int64_t time_offset;
    FuriHalRtcLocaleTimeFormat locale_timeformat;
    FuriHalRtcLocaleDateFormat locale_dateformat;
    FuriHalRtcLocaleUnits locale_units;
} FuriHalRtcState;

static FuriHalRtcState furi_hal_rtc = {
    .heap_track_mode = FuriHalRtcHeapTrackModeNone,
    .boot_mode = FuriHalRtcBootModeNormal,
    .log_device = FuriHalRtcLogDeviceNone,
    .log_baud_rate = FuriHalRtcLogBaudRate115200,
    .log_level = 0,
    .flags = 0,
    .fault_data = 0,
    .pin_fails = 0,
    .pin_value = 0,
    .time_offset = 0,
    .locale_timeformat = FuriHalRtcLocaleTimeFormat24h,
    .locale_dateformat = FuriHalRtcLocaleDateFormatDMY,
    .locale_units = FuriHalRtcLocaleUnitsMetric,
};

static time_t furi_hal_rtc_now(void) {
    return time(NULL) + (time_t)furi_hal_rtc.time_offset;
}

void furi_hal_rtc_init_early(void) {
}

void furi_hal_rtc_deinit_early(void) {
}

void furi_hal_rtc_init(void) {
    /* Restore persisted anti-theft state so lock-on-boot / wipe survive a
     * reboot or power-cycle. A missing key leaves the compiled default (0). */
    nvs_handle_t h;
    if(nvs_open(RTC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, RTC_NVS_KEY_PIN, &furi_hal_rtc.pin_value);
        nvs_get_u32(h, RTC_NVS_KEY_FAILS, &furi_hal_rtc.pin_fails);
        nvs_close(h);
    }
}

void furi_hal_rtc_prepare_for_shutdown(void) {
}

void furi_hal_rtc_sync_shadow(void) {
}

void furi_hal_rtc_reset_registers(void) {
    furi_hal_rtc.flags = 0;
    furi_hal_rtc.fault_data = 0;
    furi_hal_rtc.pin_fails = 0;
    furi_hal_rtc.pin_value = 0;
    furi_hal_rtc.boot_mode = FuriHalRtcBootModeNormal;
    /* Clear the persisted anti-theft PIN/fail state on factory reset. */
    nvs_handle_t h;
    if(nvs_open(RTC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, RTC_NVS_KEY_PIN);
        nvs_erase_key(h, RTC_NVS_KEY_FAILS);
        nvs_commit(h);
        nvs_close(h);
    }
}

void furi_hal_rtc_set_log_level(uint8_t level) {
    furi_hal_rtc.log_level = level;
}

uint8_t furi_hal_rtc_get_log_level(void) {
    return furi_hal_rtc.log_level;
}

void furi_hal_rtc_set_log_device(FuriHalRtcLogDevice device) {
    furi_hal_rtc.log_device = device;
}

FuriHalRtcLogDevice furi_hal_rtc_get_log_device(void) {
    return furi_hal_rtc.log_device;
}

void furi_hal_rtc_set_log_baud_rate(FuriHalRtcLogBaudRate baud_rate) {
    furi_hal_rtc.log_baud_rate = baud_rate;
}

FuriHalRtcLogBaudRate furi_hal_rtc_get_log_baud_rate(void) {
    return furi_hal_rtc.log_baud_rate;
}

void furi_hal_rtc_set_fault_data(uint32_t value) {
    furi_hal_rtc.fault_data = value;
}

uint32_t furi_hal_rtc_get_fault_data(void) {
    return furi_hal_rtc.fault_data;
}

void furi_hal_rtc_set_pin_fails(uint32_t value) {
    furi_hal_rtc.pin_fails = value;
    furi_hal_rtc_nvs_store(RTC_NVS_KEY_FAILS, value);
}

uint32_t furi_hal_rtc_get_pin_fails(void) {
    return furi_hal_rtc.pin_fails;
}

void furi_hal_rtc_set_pin_value(uint32_t value) {
    furi_hal_rtc.pin_value = value;
    furi_hal_rtc_nvs_store(RTC_NVS_KEY_PIN, value);
}

uint32_t furi_hal_rtc_get_pin_value(void) {
    return furi_hal_rtc.pin_value;
}

bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return (furi_hal_rtc.flags & flag) != 0;
}

void furi_hal_rtc_set_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags |= flag;
}

void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags &= ~flag;
}

FuriHalRtcBootMode furi_hal_rtc_get_boot_mode(void) {
    return furi_hal_rtc.boot_mode;
}

void furi_hal_rtc_set_boot_mode(FuriHalRtcBootMode mode) {
    furi_hal_rtc.boot_mode = mode;
}

FuriHalRtcHeapTrackMode furi_hal_rtc_get_heap_track_mode(void) {
    return furi_hal_rtc.heap_track_mode;
}

void furi_hal_rtc_set_heap_track_mode(FuriHalRtcHeapTrackMode mode) {
    furi_hal_rtc.heap_track_mode = mode;
}

void furi_hal_rtc_get_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    time_t now = furi_hal_rtc_now();
    struct tm now_tm = {0};
    localtime_r(&now, &now_tm);

    datetime->hour = now_tm.tm_hour;
    datetime->minute = now_tm.tm_min;
    datetime->second = now_tm.tm_sec;
    datetime->day = now_tm.tm_mday;
    datetime->month = now_tm.tm_mon + 1;
    datetime->year = now_tm.tm_year + 1900;
    datetime->weekday = ((now_tm.tm_wday + 6) % 7) + 1;
}

void furi_hal_rtc_set_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    struct tm desired = {
        .tm_sec = datetime->second,
        .tm_min = datetime->minute,
        .tm_hour = datetime->hour,
        .tm_mday = datetime->day,
        .tm_mon = datetime->month - 1,
        .tm_year = datetime->year - 1900,
        .tm_isdst = -1,
    };

    const time_t target = mktime(&desired);
    if(target != (time_t)-1) {
        furi_hal_rtc.time_offset = (int64_t)target - (int64_t)time(NULL);
    }
}

uint32_t furi_hal_rtc_get_timestamp(void) {
    return (uint32_t)furi_hal_rtc_now();
}

FuriHalRtcLocaleTimeFormat furi_hal_rtc_get_locale_timeformat(void) {
    return furi_hal_rtc.locale_timeformat;
}

void furi_hal_rtc_set_locale_timeformat(FuriHalRtcLocaleTimeFormat format) {
    furi_hal_rtc.locale_timeformat = format;
}

FuriHalRtcLocaleDateFormat furi_hal_rtc_get_locale_dateformat(void) {
    return furi_hal_rtc.locale_dateformat;
}

void furi_hal_rtc_set_locale_dateformat(FuriHalRtcLocaleDateFormat format) {
    furi_hal_rtc.locale_dateformat = format;
}

FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void) {
    return furi_hal_rtc.locale_units;
}

void furi_hal_rtc_set_locale_units(FuriHalRtcLocaleUnits format) {
    furi_hal_rtc.locale_units = format;
}
