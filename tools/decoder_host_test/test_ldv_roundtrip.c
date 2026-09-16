// Instrumented LDV V0 round-trip reproduction: encode a known-valid 128-bit frame, then
// measure adjacent same-level pulses / coalesce merges / re-decode progress. Isolates why
// the harness reports RT:FAIL-redecode. Build+run: bash build_test_ldv.sh
#include "../../lib/subghz/protocols/ldv_v0.c"
#include <stdio.h>
#include <stdlib.h>

static int g_hit = 0;
static void cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    (void)ctx;
    g_hit = 1;
}

int main(void) {
    // A frame that passes ldv_v0 frame_valid (btn=1: byte6=0x8F, byte7=0xFF, byte8=0xFF,
    // cnt_field=0x0C8F -> cnt_btn=1 == byte6_btn=1).
    SubGhzProtocolEncoderLdvV0* enc = subghz_protocol_encoder_ldv_v0_alloc(NULL);
    enc->generic.data = 0xBBBC57706F9A8FFFULL;
    enc->generic.data_count_bit = 128;
    enc->data_2 = 0xFF0C8F1234567890ULL;
    enc->encoder.repeat = 1;
    subghz_protocol_encoder_ldv_v0_get_upload(enc);
    size_t n = enc->encoder.size_upload;
    printf("encoder upload pulses: %zu\n", n);

    int* durs = malloc(n * sizeof(int));
    int adj = 0, adj_in_data = 0;
    // Rough: the reset trigger + preamble occupy the first pulses; the data region is the tail.
    for(size_t i = 0; i < n; i++) {
        bool lv = level_duration_get_level(enc->encoder.upload[i]);
        int d = (int)level_duration_get_duration(enc->encoder.upload[i]);
        durs[i] = lv ? d : -d;
        if(i > 0 && ((durs[i] < 0) == (durs[i - 1] < 0))) {
            adj++;
            if(i > (size_t)(LDV_V0_PREAMBLE_BITS + 4)) adj_in_data++;
        }
    }
    printf("adjacent same-level pulses (pre-coalesce): %d (in data region ~%d)\n", adj, adj_in_data);

    // Coalesce exactly like pp_decode_full roundtrip().
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

    // DEFINITIVE: feed the coalesced stream to a FRESH decoder (the real decode path),
    // then read data_2 AFTER the fire — exactly what the harness/on-device serialize sees.
    SubGhzProtocolDecoderLdvV0* dec = subghz_protocol_decoder_ldv_v0_alloc(NULL);
    subghz_protocol_decoder_base_set_decoder_callback(&dec->base, cb, NULL);
    subghz_protocol_decoder_ldv_v0_reset(dec);
    for(size_t i = 0; i < mn; i++) {
        int v = durs[i];
        subghz_protocol_decoder_ldv_v0_feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    printf("valid-frame hit = %d\n", g_hit);
    printf(
        "AFTER fire: generic.data=%016llX  data_2=%016llX (expected FF0C8F1234567890)\n",
        (unsigned long long)dec->generic.data,
        (unsigned long long)dec->data_2);
    if(dec->data_2 == 0xFF0C8F1234567890ULL) {
        printf("RESULT: data_2 PRESERVED -> serialize would be correct (fixed)\n");
    } else {
        printf("RESULT: data_2 CLOBBERED to %016llX -> serialize writes wrong Key_2 (BUG)\n",
               (unsigned long long)dec->data_2);
    }
    return 0;
}
