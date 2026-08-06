#include "subghz_mod_detect.h"
#include <furi_hal.h>

// The wide OOK-650 RX config (device layer). A WIDE filter is required: it lets an
// OOK signal swing fully on/off AND keeps an FSK carrier in-band during its
// frequency shifts, so FSK reads as a steady carrier rather than false amplitude
// dips. Declared extern to avoid an extra include path (stable device symbol).
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];

#define SMD_WINDOW_SAMPLES 200
#define SMD_SAMPLE_GAP_US  50 // ~20 kHz envelope sampling
#define SMD_SETTLE_US      2000 // RX settle after retune (matches worker's 2 ms)

// Runs inside the analyzer worker thread (which owns the radio and has released
// the SPI bus after its fine scan). Retunes to freq in the wide OOK-650 config,
// samples RSSI, classifies, and returns the radio to idle.
SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz) {
    int8_t samples[SMD_WINDOW_SAMPLES];

    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
    furi_hal_subghz_set_frequency_and_path(frequency_hz);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();
    furi_delay_us(SMD_SETTLE_US);

    for(size_t i = 0; i < SMD_WINDOW_SAMPLES; i++) {
        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > 0.f) rssi = 0.f;
        if(rssi < -128.f) rssi = -128.f;
        samples[i] = (int8_t)rssi;
        furi_delay_us(SMD_SAMPLE_GAP_US);
    }

    furi_hal_subghz_idle();
    return subghz_mod_classify(samples, SMD_WINDOW_SAMPLES);
}
