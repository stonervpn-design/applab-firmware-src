#include "subghz_sig_identify.h"
#include <furi_hal.h>
#include <string.h>

// Wide OOK / standard FSK device presets (see subghz_mod_detect.c for the wide-filter
// rationale). Declared extern to avoid an extra include path.
extern const uint8_t subghz_device_cc1101_preset_ook_650khz_async_regs[];
extern const uint8_t subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs[];

#define SIG_CAP_SAMPLES 256
#define SIG_WINDOW_MS   120
#define SIG_MIN_SCORE   40
#define SIG_ID_FRAMES    3 // capture N frames per lock and vote (cuts single-frame misses)
#define SIG_ID_MIN_VOTES 2 // K-of-N: require this many agreeing frames to accept
#define SIG_MOD_HARD_CONF 60 // >= this mod-detect confidence -> trust mod as a hard constraint
                              // (stricter than the view's SMD_CONF_SHOW=40 display bar)

typedef struct {
    PvSample* buf;
    size_t cap;
    size_t n;
} SigCapCtx;

// Runs in the CC1101 RX ISR context — keep it minimal (append only).
static void sig_capture_cb(bool level, uint32_t duration, void* context) {
    SigCapCtx* c = context;
    if(c->n < c->cap) {
        c->buf[c->n].level = level ? 1 : 0;
        c->buf[c->n].dur = duration;
        c->n++;
    }
}

bool subghz_sig_identify_run(
    uint32_t freq,
    SignalMod mod,
    uint8_t mod_confidence,
    char* out,
    size_t cap,
    uint8_t* out_score) {
    static PvSample samples[SIG_CAP_SAMPLES]; // static: off the worker's 4 KB stack
    SigCapCtx ctx = {samples, SIG_CAP_SAMPLES, 0};

    // Confidence-gate the modulation guess (finding #22): a low-confidence OOK/FSK
    // call must not force the wrong demodulator or suppress the true protocol.
    //  - capture preset: trust the FSK preset only when the call is sure; otherwise
    //    fall back to the wide OOK 650 kHz filter (it still passes FSK edges well
    //    enough to identify, and OOK is the dominant fixed-code family).
    //  - observation mod: report UNK to the fingerprint engine when unsure so its
    //    modulation hard-filter is disabled (band + timing + bits still decide).
    const bool mod_sure = mod_confidence >= SIG_MOD_HARD_CONF;
    const SignalMod obs_mod = mod_sure ? mod : SIG_MOD_UNK;
    const uint8_t* preset = (mod_sure && mod == SIG_MOD_FM) ?
                                subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs :
                                subghz_device_cc1101_preset_ook_650khz_async_regs;

    // Capture N back-to-back frames and require K agreeing reads (K-of-N vote)
    // before labelling, so one noisy frame within the lock can't mislabel.
    SignalMatch frames[SIG_ID_FRAMES];
    for(int i = 0; i < SIG_ID_FRAMES; i++) {
        ctx.n = 0; // reset the shared capture buffer for this frame
        furi_hal_subghz_idle();
        furi_hal_subghz_load_custom_preset(preset);
        furi_hal_subghz_set_frequency_and_path(freq);
        furi_hal_subghz_flush_rx();
        furi_hal_subghz_start_async_rx(sig_capture_cb, &ctx);
        furi_delay_ms(SIG_WINDOW_MS);
        furi_hal_subghz_stop_async_rx();
        frames[i] = subghz_sig_identify_from_samples(freq, obs_mod, samples, ctx.n);
    }
    furi_hal_subghz_idle();

    SignalMatch m = subghz_sig_aggregate(frames, SIG_ID_FRAMES, SIG_ID_MIN_VOTES, SIG_MIN_SCORE);

    if(out && cap) {
        const char* label = (m.make && m.make[0]) ? m.make : m.name;
        if(!label) label = "?";
        strncpy(out, label, cap - 1);
        out[cap - 1] = '\0';
    }
    bool ok = m.name && strcmp(m.name, "UNKNOWN") != 0 && m.score >= SIG_MIN_SCORE;
    if(out_score) *out_score = ok ? (uint8_t)(m.score > 100 ? 100 : m.score) : 0;
    return ok;
}
