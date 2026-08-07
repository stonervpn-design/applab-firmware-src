#include "subghz_spectrum_worker.h"
#include "subghz_spectrum_util.h"
#include <lib/drivers/cc1101.h>
#include <furi_hal_subghz.h>
#include <furi.h>

#define TAG "SubGhzSpectrumWorker"

#define SPECTRUM_SETTLE_US    200 // per-bin PLL/AGC settle (no per-bin calibration)
#define SPECTRUM_DEFAULT_BAND 433050000u // 433.05 MHz
#define SPECTRUM_DEFAULT_STEP 13600u // 128 * 13.6 kHz = ~1.74 MHz span (433.05..434.79)

// Wide ~650 kHz RX filter so a bin captures energy across its ~13.6 kHz spacing.
static const uint8_t spectrum_preset_ook_650khz[][2] = {
    {CC1101_MDMCFG4, 0b00010111},
    {0, 0},
};

struct SubGhzSpectrumWorker {
    FuriThread* thread;
    volatile bool worker_running;
    volatile uint32_t band_start;
    uint32_t bin_step;
    SubGhzSpectrumWorkerRowCallback cb;
    void* context;
};

static void spectrum_load_registers(const uint8_t data[][2]) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    size_t i = 0;
    while(data[i][0]) {
        cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, data[i][0], data[i][1]);
        i++;
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static void spectrum_set_path(uint32_t f) {
    if(f >= 281000000 && f <= 361000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    else if(f >= 378000000 && f <= 481000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    else if(f >= 749000000 && f <= 962000000)
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    else
        furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
}

static int32_t subghz_spectrum_worker_thread(void* context) {
    SubGhzSpectrumWorker* instance = context;
    uint8_t bins[SPECTRUM_NBINS];

    // One-time setup: same front-end config as the frequency analyzer, plus
    // FS_AUTOCAL off (MCSM0) so IDLE->RX does not recalibrate every bin.
    furi_hal_subghz_reset();
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, 0b00110000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MCSM0, 0b00001000); // FS_AUTOCAL=0
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    spectrum_load_registers(spectrum_preset_ook_650khz);

    while(instance->worker_running) {
        const uint32_t band = instance->band_start;
        const uint32_t step = instance->bin_step;
        const uint32_t center = band + step * (SPECTRUM_NBINS / 2);

        spectrum_set_path(center);

        // Calibrate ONCE for this band (valid across the ~1.74 MHz span).
        furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
        cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
        cc1101_set_frequency(&furi_hal_spi_bus_handle_subghz, center);
        cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
        furi_check(
            cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 10000));
        furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

        float peak_rssi = -127.0f;
        uint32_t peak_freq = band;

        for(size_t i = 0; i < SPECTRUM_NBINS && instance->worker_running; i++) {
            const uint32_t f = spectrum_bin_freq(band, step, i);
            furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
            cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
            cc1101_set_frequency(&furi_hal_spi_bus_handle_subghz, f);
            cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
            furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

            furi_delay_us(SPECTRUM_SETTLE_US);
            const float rssi = furi_hal_subghz_get_rssi();
            bins[i] = spectrum_rssi_q(rssi, SPECTRUM_BAR_H);
            if(rssi > peak_rssi) {
                peak_rssi = rssi;
                peak_freq = f;
            }
        }

        if(instance->worker_running && instance->cb) {
            instance->cb(
                instance->context, band, step, bins, SPECTRUM_NBINS, peak_freq, peak_rssi);
        }
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

SubGhzSpectrumWorker* subghz_spectrum_worker_alloc(void) {
    SubGhzSpectrumWorker* instance = malloc(sizeof(SubGhzSpectrumWorker));
    instance->worker_running = false;
    instance->band_start = SPECTRUM_DEFAULT_BAND;
    instance->bin_step = SPECTRUM_DEFAULT_STEP;
    instance->cb = NULL;
    instance->context = NULL;
    instance->thread =
        furi_thread_alloc_ex("SubGhzSpecWorker", 4096, subghz_spectrum_worker_thread, instance);
    return instance;
}

void subghz_spectrum_worker_free(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_thread_free(instance->thread);
    free(instance);
}

void subghz_spectrum_worker_set_row_callback(
    SubGhzSpectrumWorker* instance,
    SubGhzSpectrumWorkerRowCallback cb,
    void* ctx) {
    furi_assert(instance);
    instance->cb = cb;
    instance->context = ctx;
}

void subghz_spectrum_worker_start(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);
    instance->worker_running = true;
    furi_thread_start(instance->thread);
}

void subghz_spectrum_worker_stop(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);
    instance->worker_running = false;
    furi_thread_join(instance->thread);
}

bool subghz_spectrum_worker_is_running(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

void subghz_spectrum_worker_set_band_start(SubGhzSpectrumWorker* instance, uint32_t band_start) {
    furi_assert(instance);
    instance->band_start = band_start;
}

uint32_t subghz_spectrum_worker_get_band_start(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    return instance->band_start;
}

uint32_t subghz_spectrum_worker_get_bin_step(SubGhzSpectrumWorker* instance) {
    furi_assert(instance);
    return instance->bin_step;
}
