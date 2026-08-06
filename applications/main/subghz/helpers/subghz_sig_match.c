#include "subghz_sig_identify.h"
#include "protoview_decode.h"

#define SIG_MIN_SAMPLES 16
#define SIG_BITS_CAP    256

// Pure: no hardware. Reuses the (host-tested) protoview TE estimator + bit
// decoders and the (host-tested) signal_fingerprint classifier.
SignalMatch subghz_sig_identify_from_samples(
    uint32_t freq,
    SignalMod mod,
    const PvSample* s,
    size_t n) {
    SignalMatch m = {0};
    m.name = "UNKNOWN";
    if(!s || n < SIG_MIN_SAMPLES) return m;

    PvThresholds th;
    if(!pv_classify_symbols(s, n, &th)) return m; // no clear short/long TE clusters

    char bits[SIG_BITS_CAP];
    int nb_pwm = pv_decode_pwm(s, n, &th, bits, sizeof(bits));
    int nb_man = pv_decode_manchester(s, n, &th, bits, sizeof(bits));
    int nb = nb_pwm > nb_man ? nb_pwm : nb_man; // best-effort bit-count estimate
    if(nb < 0) nb = 0;

    SignalObservation obs = {
        .freq = freq,
        .mod = mod,
        .bits = nb,
        .te_short = th.t_short,
        .te_long = th.t_long,
        .name = NULL, // raw signal — no authoritative decoder name
    };

    SignalMatch matches[3];
    size_t cnt = signal_fingerprint_classify(&obs, matches, 3);
    if(cnt > 0) m = matches[0];
    return m;
}
