// Verbatim-replay round-trip for land_rover_rke: build the encoder upload from a
// known 66-bit frame (V[65:0]=extra_bits:data), feed the waveform back through
// the decoder, and confirm serial/btn/hop/extra_bits come back byte-identical.
#include "../../lib/subghz/protocols/land_rover_rke.c"
#include <stdio.h>

static int g_hit = 0;
static void cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base; (void)ctx; g_hit = 1;
}

int main(void) {
    // Synthesize a frame with a known serial/button/hop/flags/status.
    // Layout (MSB-first V[65:0]): hop[31:0]=extra_bits[1:0]<<30 | data[63:34];
    // serial=data[33:10]; btn=data[9:6]; flags=data[5:2]; status=data[1:0].
    const uint32_t serial = 0x123456;
    const uint8_t  btn    = 0x2;   // Unlock
    const uint32_t hop    = 0xDEADBEEF;
    const uint8_t  flags  = 0x5;
    const uint8_t  status = 0x2;

    uint8_t extra_bits = (uint8_t)((hop >> 30) & 0x3U);
    uint64_t data = 0;
    data |= ((uint64_t)(hop & 0x3FFFFFFFULL)) << 34; // data[63:34]
    data |= ((uint64_t)(serial & 0xFFFFFFULL)) << 10; // data[33:10]
    data |= ((uint64_t)(btn & 0xFULL)) << 6;          // data[9:6]
    data |= ((uint64_t)(flags & 0xFULL)) << 2;        // data[5:2]
    data |= ((uint64_t)(status & 0x3ULL));            // data[1:0]

    SubGhzProtocolEncoderLandRoverRke* enc =
        subghz_protocol_encoder_land_rover_rke_alloc(NULL);
    enc->data = data;
    enc->extra_bits = extra_bits;
    if(!land_rover_rke_encoder_build_upload(enc)) {
        printf("FAIL: build_upload returned false\n");
        return 1;
    }
    size_t n = enc->encoder.size_upload;
    printf("upload pulses: %zu (cap=%u)\n", n, (unsigned)LAND_ROVER_RKE_UPLOAD_CAPACITY);

    SubGhzProtocolDecoderLandRoverRke* dec =
        subghz_protocol_decoder_land_rover_rke_alloc(NULL);
    subghz_protocol_decoder_base_set_decoder_callback(&dec->base, cb, NULL);
    subghz_protocol_decoder_land_rover_rke_reset(dec);
    for(size_t i = 0; i < n; i++) {
        bool lv = level_duration_get_level(enc->encoder.upload[i]);
        uint32_t d = level_duration_get_duration(enc->encoder.upload[i]);
        subghz_protocol_decoder_land_rover_rke_feed(dec, lv, d);
    }

    printf("hit=%d serial=0x%06lX btn=0x%X hop=0x%08lX flags=0x%X status=0x%X extra=0x%X\n",
        g_hit, (unsigned long)dec->serial, dec->button, (unsigned long)dec->hop,
        dec->flags, dec->status, dec->extra_bits);

    int ok = g_hit && dec->serial == serial && dec->button == btn && dec->hop == hop &&
             dec->flags == flags && dec->status == status && dec->extra_bits == extra_bits;
    printf(ok ? "RESULT: verbatim replay round-trips OK\n"
              : "RESULT: FAIL (mismatch)\n");
    return ok ? 0 : 1;
}
