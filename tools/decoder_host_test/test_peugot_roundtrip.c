// Instrumented Peugot V0 round-trip reproduction with the REAL captured frame
// (Key:FFAD3C09C7E1F94A Key2:A6FC). Encode -> coalesce -> re-decode and report where it breaks.
#include "../../lib/subghz/protocols/peugot_v0.c"
#include <stdio.h>
#include <stdlib.h>

static int g_hit = 0;
static void cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    (void)ctx;
    g_hit = 1;
}

int main(void) {
    SubGhzProtocolEncoderPeugotV0* enc = subghz_protocol_encoder_peugot_v0_alloc(NULL);
    enc->generic.data = 0xFFAD3C09C7E1F94AULL;
    enc->generic.data_count_bit = 80;
    enc->data_16 = 0xA6FC;
    enc->encoder.repeat = 1;
    subghz_protocol_encoder_peugot_v0_get_upload(enc);
    size_t n = enc->encoder.size_upload;
    printf("encoder upload pulses: %zu\n", n);

    int* durs = malloc(n * sizeof(int));
    int adj = 0;
    for(size_t i = 0; i < n; i++) {
        bool lv = level_duration_get_level(enc->encoder.upload[i]);
        int d = (int)level_duration_get_duration(enc->encoder.upload[i]);
        durs[i] = lv ? d : -d;
        if(i > 0 && ((durs[i] < 0) == (durs[i - 1] < 0))) adj++;
    }
    printf("adjacent same-level pulses (pre-coalesce): %d\n", adj);

    size_t mn = 0;
    for(size_t k = 0; k < n; k++) {
        if(mn > 0 && ((durs[k] < 0) == (durs[mn - 1] < 0))) {
            int sign = durs[mn - 1] < 0 ? -1 : 1;
            long mag = (long)abs(durs[mn - 1]) + (long)abs(durs[k]);
            durs[mn - 1] = sign * (int)mag;
        } else {
            durs[mn++] = durs[k];
        }
    }
    printf("coalesced pulses: %zu (merges: %zu)\n", mn, n - mn);

    SubGhzProtocolDecoderPeugotV0* dec = subghz_protocol_decoder_peugot_v0_alloc(NULL);
    subghz_protocol_decoder_base_set_decoder_callback(&dec->base, cb, NULL);
    subghz_protocol_decoder_peugot_v0_reset(dec);
    for(size_t i = 0; i < mn; i++) {
        int v = durs[i];
        subghz_protocol_decoder_peugot_v0_feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    // The decoder processes on a gap; feed one more big gap to flush.
    subghz_protocol_decoder_peugot_v0_feed(dec, false, 20000);
    printf("re-decode: bits collected(last)=%u, hit=%d\n", dec->bit_count, g_hit);
    printf(
        "re-decoded: Key=%016llX Key2=%04X (expected FFAD3C09C7E1F94A / A6FC)\n",
        (unsigned long long)dec->generic.data,
        dec->data_16);
    if(g_hit && dec->generic.data == 0xFFAD3C09C7E1F94AULL && dec->data_16 == 0xA6FC)
        printf("RESULT: round-trips OK (fixed)\n");
    else
        printf("RESULT: FAILS (reproduced)\n");
    return 0;
}
