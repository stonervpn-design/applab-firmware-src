#include "subghz_fob_analyzer_worker.h"
#include <lib/drivers/cc1101.h>
#include <lib/subghz/devices/cc1101_configs.h> // async OOK preset reg arrays
#include <furi_hal_subghz.h>

#include <furi.h>
#include <math.h>
#include <string.h>
#include <float_tools.h>

#define TAG "SubghzFobAnalyzerWorker"

// --- Carrier-lock reliability (mirrors the stock frequency analyzer) ---
#define FOB_SAMPLE_HOLD 20 // hold a lock this many passes so a brief burst stays on screen
// AGCCTRL0: [7:6]HYST [5:4]WAIT [3:2]AGC_FREEZE [1:0]FILTER — scan uses auto-AGC.
#define FOB_AGCCTRL0_SCAN 0b00110000

// --- Per-frequency baseline delta detection (the real fob discriminator) ---
// "Loudest frequency wins" locks onto persistent interferers (a 320MHz spur that
// is always ~-67dBm beats an intermittent fob's quiet gaps). Instead, each
// frequency keeps a baseline that tracks its OWN noise floor: a persistent signal
// sits at delta~0 and is ignored, while a fob spiking above its baseline wins.
#define FOB_MAX_FREQS      128 // upper bound on the region frequency-list length
#define FOB_BASELINE_SENT  1.0f // sentinel (RSSI is always < 0) = "not yet seeded"
#define FOB_BASELINE_LEAK  0.4f // dB/pass the baseline leaks UP (snaps DOWN to new lows)
#define FOB_DELTA_MARGIN   12.0f // a freq must rise this many dB over its baseline to qualify
// AND clear this absolute floor: a fob held to the device reads ~-40..-48 dBm, while
// ambient chatter (weather stations, TPMS, distant remotes) sits at -55 and below.
// Requiring a strong signal makes the analyzer show "hunting" until a fob is actually
// pressed close, instead of locking onto the loudest background transmitter.
#define FOB_ABS_LOCK       -54.0f
#define FOB_WARMUP_PASSES  3 // let baselines settle before the first lock is allowed
#define FOB_STICKY_BONUS   8.0f // hysteresis: keep the held carrier unless another beats it by this
#define FOB_STICKY_HZ      250000u // "same carrier" window for the hysteresis bonus

// --- Pulse-timing classifier (real GDO0 edge durations, not RSSI polling) ---
#define FOB_CAP_MAX_EDGES    4096u
#define FOB_CAP_WINDOW_MS    120u // one held-button repeat interval — long enough to catch a frame
#define FOB_HIST_BINS        64u
#define FOB_HIST_BIN_US      32u // covers 0..2048us; longer = inter-packet gap
#define FOB_LONGGAP_US       2048u
#define FOB_TE_MIN_US        60u
#define FOB_TE_MAX_US        1500u
#define FOB_MIN_STRUCT_EDGES 40u // >= this many quantized edges under the OOK slicer => OOK
#define FOB_BIN_FLOOR        5u

/** RF band switch for boards with an external path mux. */
static void subghz_fob_worker_set_path_for_frequency(uint32_t freq) {
    if(freq >= 281000000 && freq <= 361000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath315);
    } else if(freq >= 378000000 && freq <= 481000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath433);
    } else if(freq >= 749000000 && freq <= 962000000) {
        furi_hal_subghz_set_path(FuriHalSubGhzPath868);
    } else {
        furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);
    }
}

static const uint8_t subghz_fob_preset_ook_58khz[][2] = {
    {CC1101_MDMCFG4, 0b11110111}, // Rx BW filter 58.035714kHz
    {0, 0},
};

static const uint8_t subghz_fob_preset_ook_650khz[][2] = {
    {CC1101_MDMCFG4, 0b00010111}, // Rx BW filter 650.000kHz
    {0, 0},
};

struct SubGhzFobAnalyzerWorker {
    FuriThread* thread;
    volatile bool worker_running;
    SubGhzSetting* setting;
    float trigger_level; // absolute, from settings (~-97dBm default)
    // Reference-style sample hold + last-known lock, carried across passes.
    uint8_t sample_hold_counter;
    // Per-frequency-list-index noise baseline + warm-up counter for delta detection.
    float baseline[FOB_MAX_FREQS];
    uint16_t warmup;
    uint32_t last_frequency;
    float last_rssi;
    FobMod last_mod;
    FobEnc last_enc;
    uint32_t last_te_us;
    uint32_t last_bitrate;
    uint32_t last_fsk_dev_khz;
    SubGhzFobAnalyzerWorkerCallback callback;
    void* context;
};

static void subghz_fob_analyzer_worker_load_registers(const uint8_t data[][2]) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    size_t i = 0;
    while(data[i][0]) {
        cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, data[i][0], data[i][1]);
        i++;
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

/** Tune to a frequency and enter RX (shared by scan and the FSK probe). */
static uint32_t subghz_fob_analyzer_worker_tune_rx(uint32_t freq) {
    subghz_fob_worker_set_path_for_frequency(freq);
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
    uint32_t real = cc1101_set_frequency(&furi_hal_spi_bus_handle_subghz, freq);
    cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
    // Tolerant like the HAL's own set_frequency/idle: a transient calibration miss
    // must yield one low-RSSI sample, NOT furi_crash mid-transaction (this runs once
    // per scanned frequency, and the T-Embed's rail/band-switch settling can hiccup).
    (void)cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 10000);
    cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    return real;
}

/** (Re)establish the RSSI-scan register config. Called at the top of EVERY pass
 * because the classify step below loads an async custom preset (which resets the
 * chip and repurposes GDO0/IOCFG0); this restores the scan config each time. */
static void subghz_fob_worker_load_scan_config(void) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_rx(&furi_hal_spi_bus_handle_subghz);
    cc1101_flush_tx(&furi_hal_spi_bus_handle_subghz);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_IOCFG0, CC1101IocfgHW);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_MDMCFG3, 0b01111111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL2, 0b00000111);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL1, 0b00001000);
    cc1101_write_reg(&furi_hal_spi_bus_handle_subghz, CC1101_AGCCTRL0, FOB_AGCCTRL0_SCAN);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

// =============================================================================
// Pulse-timing capture (GDO0 async data slicer)
// =============================================================================

typedef struct {
    volatile uint32_t count;
    uint8_t level[FOB_CAP_MAX_EDGES];
    uint16_t dur[FOB_CAP_MAX_EDGES];
} FobCapture;

/** GPIO ISR context: append only — NO malloc/log/SPI/blocking. ctx = FobCapture*. */
static void fob_capture_cb(bool level, uint32_t duration, void* ctx) {
    FobCapture* cap = ctx;
    uint32_t c = cap->count;
    if(c >= FOB_CAP_MAX_EDGES) return;
    cap->level[c] = level ? 1u : 0u;
    cap->dur[c] = (duration > 0xFFFFu) ? 0xFFFFu : (uint16_t)duration;
    cap->count = c + 1u;
}

/** Open the OOK demod slicer on `freq` and collect edge durations for window_ms. */
static uint32_t fob_capture_edges(uint32_t freq, FobCapture* cap, uint32_t window_ms) {
    cap->count = 0;
    furi_hal_subghz_idle();
    // Async OOK preset: IOCFG0 = serial demod data on GDO0, async continuous mode.
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);
    subghz_fob_worker_set_path_for_frequency(freq);
    furi_hal_subghz_set_frequency(freq);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_start_async_rx(fob_capture_cb, cap); // installs GDO0 edge IRQ + RX
    furi_delay_ms(window_ms);
    furi_hal_subghz_stop_async_rx(); // removes the IRQ
    // Let any capture ISR already latched on the other core finish writing `cap`
    // before the caller frees it (stop detaches the handler but does not join it).
    furi_delay_ms(1);
    return cap->count;
}

// =============================================================================
// Pulse-width histogram + encoding classifier
// =============================================================================

typedef struct {
    uint16_t mark[FOB_HIST_BINS];
    uint16_t space[FOB_HIST_BINS];
    uint32_t struct_edges;
    uint32_t long_gaps;
} FobHist;

static void fob_hist_build(const FobCapture* cap, FobHist* h) {
    memset(h, 0, sizeof(*h));
    for(uint32_t i = 0; i < cap->count; i++) {
        uint32_t d = cap->dur[i];
        if(d > FOB_LONGGAP_US) {
            h->long_gaps++;
            continue;
        }
        if(d >= FOB_TE_MIN_US && d <= 8000u) h->struct_edges++;
        uint32_t bin = d / FOB_HIST_BIN_US;
        if(bin >= FOB_HIST_BINS) bin = FOB_HIST_BINS - 1;
        if(cap->level[i])
            h->mark[bin]++;
        else
            h->space[bin]++;
    }
}

/** Lowest populated cluster centroid (us) = shortest quantized element = TE. */
static uint32_t fob_hist_te(const uint16_t* bins) {
    uint32_t start_bin = FOB_TE_MIN_US / FOB_HIST_BIN_US;
    for(uint32_t b = start_bin; b < FOB_HIST_BINS; b++) {
        if(bins[b] >= FOB_BIN_FLOOR) {
            // Don't fold the sub-TE_MIN neighbour (bin below start_bin, populated by
            // short glitch edges) into the centroid — it drags TE below the floor.
            uint32_t lo = (b > start_bin) ? bins[b - 1] : 0;
            uint32_t hi = (b + 1 < FOB_HIST_BINS) ? bins[b + 1] : 0;
            uint32_t den = lo + bins[b] + hi;
            if(!den) return b * FOB_HIST_BIN_US + FOB_HIST_BIN_US / 2;
            uint32_t num = (b - 1) * lo + b * bins[b] + (b + 1) * hi;
            return (num * FOB_HIST_BIN_US) / den + FOB_HIST_BIN_US / 2;
        }
    }
    return 0;
}

static uint32_t fob_hist_clusters(const uint16_t* bins) {
    uint16_t peak = 0;
    for(uint32_t b = 0; b < FOB_HIST_BINS; b++)
        if(bins[b] > peak) peak = bins[b];
    if(peak < FOB_BIN_FLOOR) return 0;
    uint16_t thr = (peak / 4u > FOB_BIN_FLOOR) ? peak / 4u : FOB_BIN_FLOOR;
    uint32_t clusters = 0;
    bool in = false;
    for(uint32_t b = 0; b < FOB_HIST_BINS; b++) {
        if(bins[b] >= thr) {
            if(!in) {
                clusters++;
                in = true;
            }
        } else {
            in = false;
        }
    }
    return clusters;
}

static FobEnc fob_classify_encoding(const FobHist* h) {
    uint32_t mc = fob_hist_clusters(h->mark);
    uint32_t sc = fob_hist_clusters(h->space);
    if(mc == 1 && sc >= 2) return FobEncPpm;
    if(mc >= 2 && sc >= 2) {
        // PWM keeps mark+space ~constant (asymmetric rows); Manchester's mark and
        // space rows are near-mirror images (symmetric). Use row symmetry to split.
        uint32_t diff = 0, tot = 0;
        for(uint32_t b = 0; b < FOB_HIST_BINS; b++) {
            uint32_t m = h->mark[b], s = h->space[b];
            diff += (m > s) ? (m - s) : (s - m);
            tot += m + s;
        }
        if(tot && (diff * 100u / tot) < 25u) return FobEncManchester;
        return FobEncPwm;
    }
    return FobEncUnknown;
}

// =============================================================================
// FSK sideband deviation probe
// =============================================================================

/** OOK keeps energy centered; 2-FSK splits it to f0±deviation. Sweep off-center
 * RSSI to bucket the deviation (narrow vs wide). */
static FobMod
    fob_fsk_sideband_probe(SubGhzFobAnalyzerWorker* inst, uint32_t f0, uint32_t* dev_khz_out) {
    static const int32_t off[] = {0, 15000, 30000, 47000};
    float r[4];
    furi_hal_subghz_idle();
    subghz_fob_analyzer_worker_load_registers(subghz_fob_preset_ook_58khz);
    for(uint32_t k = 0; k < 4; k++) {
        float acc = 0;
        uint32_t n = 0;
        int32_t taps = off[k] ? 2 : 1;
        for(int32_t s = 0; s < taps; s++) {
            uint32_t f = (s == 0) ? f0 + off[k] : f0 - off[k];
            if(!furi_hal_subghz_is_frequency_valid(f)) continue;
            subghz_fob_analyzer_worker_tune_rx(f);
            furi_delay_ms(2);
            acc += furi_hal_subghz_get_rssi();
            n++;
        }
        r[k] = n ? acc / (float)n : -127.0f;
    }
    *dev_khz_out = 0;
    if(r[0] < inst->trigger_level) return FobModFsk;
    // Require the wide (±47k) sideband to also beat the narrow (±15k) tap, so a
    // signal whose energy concentrates near ±15k isn't mis-bucketed wide by outer
    // leakage. Check narrow only after wide is ruled out.
    if(r[3] >= r[0] - 6.0f && r[3] >= r[1]) {
        *dev_khz_out = 47;
        return FobModFskWide;
    }
    if(r[1] >= r[0] - 6.0f) {
        *dev_khz_out = 15;
        return FobModFskNarrow;
    }
    return FobModFsk;
}

// =============================================================================
// Classify on an already-located carrier via real pulse timing.
// Returns false if the signal was gone (caller keeps its immediate report).
// =============================================================================
static bool subghz_fob_analyzer_worker_classify(
    SubGhzFobAnalyzerWorker* instance,
    uint32_t freq,
    FobAnalyzerResult* out) {
    FobCapture* cap = malloc(sizeof(FobCapture)); // ~12KB, off the 8K worker stack
    if(!cap) return false;
    fob_capture_edges(freq, cap, FOB_CAP_WINDOW_MS);
    FobHist h;
    fob_hist_build(cap, &h);
    free(cap);

    out->frequency = freq;
    out->signal = true;
    out->te_us = 0;
    out->bitrate = 0;
    out->enc = FobEncUnknown;
    out->fsk_dev_khz = 0;
    out->spread_db = 0.0f;

    if(h.struct_edges >= FOB_MIN_STRUCT_EDGES) {
        // Many quantized on/off edges under the OOK slicer => amplitude keying.
        out->mod = FobModOok;
        uint32_t te_m = fob_hist_te(h.mark);
        uint32_t te_s = fob_hist_te(h.space);
        uint32_t te = te_m;
        if(te_s && (!te || te_s < te)) te = te_s;
        out->enc = fob_classify_encoding(&h);
        if(te >= FOB_TE_MIN_US && te <= FOB_TE_MAX_US) {
            out->te_us = te;
            uint32_t bitper = (out->enc == FobEncPpm) ? (te * 3u) : (te * 2u);
            out->bitrate = bitper ? 1000000u / bitper : 0;
        }
        FURI_LOG_D(
            TAG,
            "OOK %lu Hz edges=%lu te=%lu enc=%d",
            freq,
            h.struct_edges,
            out->te_us,
            (int)out->enc);
        return true;
    }

    // Few keyed edges under the OOK slicer => steady carrier => FSK. Probe
    // sidebands for a coarse deviation bucket, then re-confirm the carrier is
    // still up (reject a signal that faded during the probe).
    uint32_t dev = 0;
    out->mod = fob_fsk_sideband_probe(instance, freq, &dev);
    out->fsk_dev_khz = dev;
    furi_hal_subghz_idle();
    subghz_fob_analyzer_worker_load_registers(subghz_fob_preset_ook_58khz);
    subghz_fob_analyzer_worker_tune_rx(freq);
    furi_delay_ms(2);
    if(furi_hal_subghz_get_rssi() < instance->trigger_level) return false;
    FURI_LOG_D(TAG, "FSK %lu Hz edges=%lu mod=%d dev=%lukHz", freq, h.struct_edges, (int)out->mod, dev);
    return true;
}

static int32_t subghz_fob_analyzer_worker_thread(void* context) {
    SubGhzFobAnalyzerWorker* instance = context;

    furi_hal_subghz_reset();
    furi_hal_subghz_set_path(FuriHalSubGhzPathIsolate);

    instance->sample_hold_counter = 0;
    instance->last_mod = FobModUnknown;
    instance->last_enc = FobEncUnknown;
    instance->last_te_us = 0;
    instance->last_bitrate = 0;
    instance->last_fsk_dev_khz = 0;
    instance->warmup = FOB_WARMUP_PASSES;
    for(size_t i = 0; i < FOB_MAX_FREQS; i++) instance->baseline[i] = FOB_BASELINE_SENT;

    while(instance->worker_running) {
        furi_delay_ms(10);

        uint32_t coarse_freq = 0, fine_freq = 0;
        float coarse_rssi = -127.0f, fine_rssi = -127.0f;
        float noise_sum = 0.0f;
        size_t noise_n = 0;
        // Baseline-delta candidate: the frequency rising the most above its OWN floor.
        uint32_t cand_freq = 0;
        float cand_rssi = -127.0f;
        float cand_delta = 0.0f;
        float cand_base = 0.0f;
        float cand_eff = -1000.0f; // best effective RSSI (incl. hysteresis bonus)

        // --- restore scan config (classify may have loaded an async preset) ---
        furi_hal_subghz_idle();
        subghz_fob_worker_load_scan_config();

        // --- coarse scan (wide 650kHz): per-frequency baseline + delta ---
        subghz_fob_analyzer_worker_load_registers(subghz_fob_preset_ook_650khz);
        size_t count = subghz_setting_get_frequency_count(instance->setting);
        for(size_t i = 0; i < count; i++) {
            uint32_t f = subghz_setting_get_frequency(instance->setting, i);
            if(!furi_hal_subghz_is_frequency_valid(f) || f > 920000000 || f == 462750000 ||
               f == 467750000 || f == 464000000) {
                continue;
            }
            uint32_t real = subghz_fob_analyzer_worker_tune_rx(f);
            furi_delay_ms(2);
            float r = furi_hal_subghz_get_rssi();
            noise_sum += r;
            noise_n++;
            if(r > coarse_rssi) {
                coarse_rssi = r;
                coarse_freq = real;
            }
            // Update this frequency's baseline: leak UP slowly, snap DOWN to new lows,
            // so it tracks the noise floor. A PERSISTENT interferer sits at delta~0;
            // an intermittent fob spikes well above its own baseline and wins.
            if(i < FOB_MAX_FREQS) {
                float bl = instance->baseline[i];
                if(bl > FOB_BASELINE_SENT - 0.5f) {
                    bl = r; // first sighting — seed the baseline
                } else {
                    bl += FOB_BASELINE_LEAK;
                    if(r < bl) bl = r;
                }
                instance->baseline[i] = bl;
                float delta = r - bl;
                // Among signals that rose FOB_DELTA_MARGIN over their own baseline,
                // pick the LOUDEST (a fob pressed close is the strongest transient).
                // A hysteresis bonus for the currently-held carrier stops the display
                // hopping between competing intermittent emitters once it caught the fob.
                if(r > FOB_ABS_LOCK && delta >= FOB_DELTA_MARGIN) {
                    float eff = r;
                    uint32_t df = (real > instance->last_frequency) ?
                                      (real - instance->last_frequency) :
                                      (instance->last_frequency - real);
                    if(instance->last_frequency > 0 && df < FOB_STICKY_HZ) eff += FOB_STICKY_BONUS;
                    if(eff > cand_eff) {
                        cand_eff = eff;
                        cand_delta = delta;
                        cand_freq = real;
                        cand_rssi = r;
                        cand_base = bl;
                    }
                }
            }
        }

        float noise_floor = (noise_n) ? (noise_sum / (float)noise_n) : instance->trigger_level;
        if(instance->warmup) instance->warmup--;

        // A candidate exists iff some frequency qualified above (delta + floor) and
        // warm-up has passed so the baselines are meaningful.
        bool have_cand = (instance->warmup == 0) && cand_freq > 0;

        // --- fine scan (narrow 58kHz) around the candidate for a precise carrier ---
        if(have_cand) {
            furi_hal_subghz_idle();
            subghz_fob_analyzer_worker_load_registers(subghz_fob_preset_ook_58khz);
            for(uint32_t f = cand_freq - 300000; f < cand_freq + 300000; f += 20000) {
                if(!furi_hal_subghz_is_frequency_valid(f)) continue;
                uint32_t real = subghz_fob_analyzer_worker_tune_rx(f);
                furi_delay_ms(2);
                float r = furi_hal_subghz_get_rssi();
                if(r > fine_rssi) {
                    fine_rssi = r;
                    fine_freq = real;
                }
            }
        }

        // --- lock decision (reference sample_hold pattern) ---
        bool locked = false;
        uint32_t lock_freq = 0;
        float lock_rssi = -127.0f;
        if(have_cand && fine_freq > 0) {
            locked = true;
            lock_freq = fine_freq;
            lock_rssi = (fine_rssi > cand_rssi) ? fine_rssi : cand_rssi;
        } else if(have_cand) {
            locked = true;
            lock_freq = cand_freq;
            lock_rssi = cand_rssi;
        }

        // Per-pass diagnostic (debug level — enable SubGhz debug logs to inspect the
        // baseline/delta/lock decision if detection ever misbehaves in a new environment).
        FURI_LOG_D(
            TAG,
            "pass loud=%luHz@%d cand=%luHz@%d base=%d delta=%d noise=%d locked=%d lockf=%luHz",
            (unsigned long)coarse_freq,
            (int)coarse_rssi,
            (unsigned long)cand_freq,
            (int)cand_rssi,
            (int)cand_base,
            (int)cand_delta,
            (int)noise_floor,
            (int)locked,
            (unsigned long)lock_freq);

        if(locked) {
            // A new carrier's last classification belongs to a DIFFERENT frequency;
            // clear it so report-first doesn't pair this freq with a stale mod/TE
            // until classify-second fills in the real values for this carrier.
            if(lock_freq != instance->last_frequency) {
                instance->last_mod = FobModUnknown;
                instance->last_enc = FobEncUnknown;
                instance->last_te_us = 0;
                instance->last_bitrate = 0;
                instance->last_fsk_dev_khz = 0;
            }
            instance->sample_hold_counter = FOB_SAMPLE_HOLD;
            instance->last_frequency = lock_freq;
            instance->last_rssi = lock_rssi;

            // (1) REPORT-FIRST: freq+rssi shown immediately, carrying last-known mod.
            FobAnalyzerResult r = {0};
            r.signal = true;
            r.frequency = lock_freq;
            r.rssi = lock_rssi;
            r.mod = instance->last_mod;
            r.enc = instance->last_enc;
            r.te_us = instance->last_te_us;
            r.bitrate = instance->last_bitrate;
            r.fsk_dev_khz = instance->last_fsk_dev_khz;
            if(instance->callback) instance->callback(instance->context, &r);

            // (2) CLASSIFY-SECOND: refine on the SAME carrier while the burst is up.
            FobAnalyzerResult c = {0};
            if(subghz_fob_analyzer_worker_classify(instance, lock_freq, &c)) {
                // classify() does not measure RSSI; carry the locked carrier's dBm
                // (a negative value can never win the old `> 0.0` max, which blanked it).
                c.rssi = lock_rssi;
                instance->last_mod = c.mod;
                instance->last_enc = c.enc;
                instance->last_te_us = c.te_us;
                instance->last_bitrate = c.bitrate;
                instance->last_fsk_dev_khz = c.fsk_dev_khz;
                instance->last_rssi = c.rssi;
                c.signal = true;
                c.frequency = lock_freq;
                if(instance->callback) instance->callback(instance->context, &c);
            }
        } else if(instance->sample_hold_counter > 0) {
            // HOLD: keep the last lock on screen (stable, no flicker).
            instance->sample_hold_counter--;
            FobAnalyzerResult r = {0};
            r.signal = true;
            r.frequency = instance->last_frequency;
            r.rssi = instance->last_rssi;
            r.mod = instance->last_mod;
            r.enc = instance->last_enc;
            r.te_us = instance->last_te_us;
            r.bitrate = instance->last_bitrate;
            r.fsk_dev_khz = instance->last_fsk_dev_khz;
            if(instance->callback) instance->callback(instance->context, &r);
        } else {
            // Expired -> hunting.
            instance->last_mod = FobModUnknown;
            instance->last_enc = FobEncUnknown;
            instance->last_te_us = 0;
            instance->last_bitrate = 0;
            instance->last_fsk_dev_khz = 0;
            FobAnalyzerResult r = {0};
            r.signal = false;
            r.frequency = 0;
            r.rssi = 0.0f; // hunting: no confirmed carrier, so no RSSI to show
            r.mod = FobModUnknown;
            if(instance->callback) instance->callback(instance->context, &r);
        }
    }

    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

SubGhzFobAnalyzerWorker* subghz_fob_analyzer_worker_alloc(void* context) {
    furi_assert(context);
    SubGhzFobAnalyzerWorker* instance = malloc(sizeof(SubGhzFobAnalyzerWorker));
    instance->worker_running = false;
    instance->callback = NULL;
    instance->context = NULL;
    instance->sample_hold_counter = 0;
    instance->last_frequency = 0;
    instance->last_rssi = 0.0f;
    instance->last_mod = FobModUnknown;
    instance->last_enc = FobEncUnknown;
    instance->last_te_us = 0;
    instance->last_bitrate = 0;
    instance->last_fsk_dev_khz = 0;
    instance->thread = furi_thread_alloc_ex(
        "SubGhzFobWorker", 8192, subghz_fob_analyzer_worker_thread, instance);
    SubGhz* subghz = context;
    instance->setting = subghz_txrx_get_setting(subghz->txrx);
    instance->trigger_level = subghz->last_settings->frequency_analyzer_trigger;
    return instance;
}

void subghz_fob_analyzer_worker_free(SubGhzFobAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_thread_free(instance->thread);
    free(instance);
}

void subghz_fob_analyzer_worker_set_callback(
    SubGhzFobAnalyzerWorker* instance,
    SubGhzFobAnalyzerWorkerCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->context = context;
}

void subghz_fob_analyzer_worker_start(SubGhzFobAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(!instance->worker_running);
    instance->worker_running = true;
    furi_thread_start(instance->thread);
}

void subghz_fob_analyzer_worker_stop(SubGhzFobAnalyzerWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->worker_running);
    instance->worker_running = false;
    furi_thread_join(instance->thread);
}

bool subghz_fob_analyzer_worker_is_running(SubGhzFobAnalyzerWorker* instance) {
    furi_assert(instance);
    return instance->worker_running;
}

void subghz_fob_analyzer_worker_set_trigger_level(SubGhzFobAnalyzerWorker* instance, float value) {
    instance->trigger_level = value;
}

float subghz_fob_analyzer_worker_get_trigger_level(SubGhzFobAnalyzerWorker* instance) {
    return instance->trigger_level;
}
