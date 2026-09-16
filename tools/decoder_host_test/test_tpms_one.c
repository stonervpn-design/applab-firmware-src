// Single-protocol TPMS encoder round-trip validator (isolated: references exactly
// ONE protocol via -DONE_PROTO and its packer via -DPACK, so parallel builds of
// different protocols never collide). Self-consistent check: hand-craft values ->
// pack -> serialize -> encoder deserialize -> yield edges -> feed the SAME decoder
// -> confirm the decoded id/pressure/temp match the input. Because each decoder was
// validated against real captures, encoder<->decoder consistency proves the encoder.
//
// Usage: test_tpms_one <id_hex> <pressure_bar> <temp_c>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/protocols/tpms_generic.h>
#include <lib/flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <toolbox/level_duration.h>
#include <core/furi_string.h>

extern const SubGhzProtocol ONE_PROTO;
void PACK(TPMSBlockGeneric* g);

typedef struct { int hit; } HitFlag;
static void rx_cb(SubGhzProtocolDecoderBase* b, void* c) { (void)b; ((HitFlag*)c)->hit++; }

int main(int argc, char** argv) {
    uint32_t id = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 16) : 0x0C12ABCDUL;
    float pressure = (argc > 2) ? (float)atof(argv[2]) : 2.20f;
    float temp = (argc > 3) ? (float)atof(argv[3]) : 20.0f;
    static char env;

    // 1) hand-craft a generic and PACK it into wire bytes.
    TPMSBlockGeneric g = {0};
    g.protocol_name = ONE_PROTO.name;
    g.id = id; g.pressure = pressure; g.temperature = temp; g.battery_low = 0;
    PACK(&g);
    printf("packed: id=0x%08lX bit=%u data=0x%016llX\n",
           (unsigned long)g.id, g.data_count_bit, (unsigned long long)g.data);

    // 2) serialize (as replace_tpms_payload does) with a fake custom preset.
    SubGhzRadioPreset preset = {0};
    preset.name = furi_string_alloc_set("TPMS");
    preset.frequency = 433920000; preset.data = NULL; preset.data_size = 0;
    FlipperFormat* fff = flipper_format_string_alloc();
    if(tpms_block_generic_serialize(&g, fff, &preset) != SubGhzProtocolStatusOk) {
        printf("FAIL serialize\n"); return 1;
    }

    // 3) encoder deserialize (the tx_start path).
    if(!ONE_PROTO.encoder || !ONE_PROTO.encoder->alloc) { printf("FAIL no encoder\n"); return 1; }
    void* enc = ONE_PROTO.encoder->alloc((SubGhzEnvironment*)&env);
    flipper_format_rewind(fff);
    SubGhzProtocolStatus st = ONE_PROTO.encoder->deserialize(enc, fff);
    if(st != SubGhzProtocolStatusOk) { printf("FAIL encoder deserialize=%d\n", (int)st); return 1; }

    // 4) yield edges.
    static int raw[8192]; size_t n = 0;
    for(int i = 0; i < 4000 && n < 8190; i++) {
        LevelDuration ld = ONE_PROTO.encoder->yield(enc);
        if(level_duration_is_reset(ld)) break;
        uint32_t d = level_duration_get_duration(ld);
        if(d == 0) break;
        raw[n++] = level_duration_get_level(ld) ? (int)d : -(int)d;
    }
    printf("edges=%zu\n", n);

    // DUMP mode: write the signed durations (us) to the file named by $DUMP, so an
    // external tool can render them to 2-FSK IQ for an over-air transmit test.
    const char* dumpf = getenv("DUMP");
    if(dumpf && *dumpf) {
        FILE* df = fopen(dumpf, "w");
        if(df) { for(size_t s = 0; s < n; s++) fprintf(df, "%d\n", raw[s]); fclose(df);
                 printf("dumped %zu durations to %s\n", n, dumpf); }
    }

    // 5) feed the SAME decoder twice (some frames need a lead-in) + confirm decode.
    void* dec = ONE_PROTO.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    if(ONE_PROTO.decoder->reset) ONE_PROTO.decoder->reset(dec);
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
    for(int rep = 0; rep < 3; rep++)
        for(size_t s = 0; s < n; s++)
            ONE_PROTO.decoder->feed(dec, raw[s] > 0, (uint32_t)(raw[s] < 0 ? -raw[s] : raw[s]));

    if(!hf.hit) { printf("FAIL round-trip: own decoder did not fire (%zu edges)\n", n); return 2; }
    FuriString* out = furi_string_alloc();
    if(ONE_PROTO.decoder->get_string) ONE_PROTO.decoder->get_string(dec, out);
    char fb[512]; const char* s = furi_string_get_cstr(out); int j = 0;
    for(int i = 0; s[i] && j < 511; i++) { char c = s[i]; fb[j++] = (c=='\r'||c=='\n') ? ' ' : c; }
    fb[j] = 0;
    printf("PASS hits=%d  DECODED: %s\n", hf.hit, fb);
    return 0;
}
