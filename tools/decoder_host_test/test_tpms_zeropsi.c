// Mirrors the device's TPMS 0-PSI flat-tire spoof path (subghz_scene_receiver_info
// set_zero_psi -> replace_tpms_payload -> tx_start): deserialize a picked-up
// sensor, zero the pressure, re-pack per-protocol, re-serialize (as
// replace_tpms_payload does via tpms_block_generic_serialize), then drive the
// encoder deserialize (the tx_start path) and feed every decoder to confirm the
// transmitted 0-PSI frame is still decodable (and reports ~0 bar).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/protocols/tpms_generic.h>
#include <lib/subghz/protocols/tpms_jeep.h>
#include <lib/subghz/protocols/tpms_citroen.h>
#include <lib/flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <toolbox/level_duration.h>
#include <core/furi_string.h>

extern const SubGhzProtocol tpms_protocol_ford, tpms_protocol_toyota, tpms_protocol_pmv107j,
    tpms_protocol_honda, tpms_protocol_nissan, tpms_protocol_hyundai_vdo, tpms_protocol_renault,
    tpms_protocol_citroen, tpms_protocol_abarth124, tpms_protocol_jansite, tpms_protocol_jeep,
    tpms_protocol_truck, tpms_protocol_porsche, tpms_protocol_bmw, tpms_protocol_bmw_g3,
    tpms_protocol_schrader_eg53ma4, tpms_protocol_schrader_gg4;
static const SubGhzProtocol* ALLDEC[] = {
    &tpms_protocol_ford, &tpms_protocol_toyota, &tpms_protocol_pmv107j, &tpms_protocol_honda,
    &tpms_protocol_nissan, &tpms_protocol_hyundai_vdo, &tpms_protocol_renault, &tpms_protocol_citroen,
    &tpms_protocol_abarth124, &tpms_protocol_jansite, &tpms_protocol_jeep, &tpms_protocol_truck,
    &tpms_protocol_porsche, &tpms_protocol_bmw, &tpms_protocol_bmw_g3, &tpms_protocol_schrader_eg53ma4,
    &tpms_protocol_schrader_gg4};
#define NDEC (sizeof(ALLDEC) / sizeof(ALLDEC[0]))

typedef struct { int hit; } HitFlag;
static void rx_cb(SubGhzProtocolDecoderBase* b, void* c) { (void)b; ((HitFlag*)c)->hit++; }

static FlipperFormat* load_file(const char* path) {
    FILE* fp = fopen(path, "rb"); if(!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = malloc(n + 1); fread(buf, 1, n, fp); buf[n] = 0; fclose(fp);
    FlipperFormat* ff = flipper_format_string_alloc();
    stream_write_cstring(flipper_format_get_raw_stream(ff), buf); free(buf);
    flipper_format_rewind(ff); return ff;
}
static void flat(const char* s, char* o, int m){int j=0;for(int i=0;s[i]&&j<m-1;i++){char c=s[i];if(c=='\r'||c=='\n')c=' ';o[j++]=c;}o[j]=0;}

int main(int argc, char** argv) {
    if(argc < 3) { fprintf(stderr, "usage: %s <key.sub> <proto_name>\n", argv[0]); return 2; }
    const char* keypath = argv[1]; const char* pname = argv[2];
    const SubGhzProtocol* ep = NULL;
    for(size_t i = 0; i < NDEC; i++) if(!strcmp(ALLDEC[i]->name, pname)) ep = ALLDEC[i];
    if(!ep || !ep->encoder || !ep->encoder->alloc) { printf("no encoder for '%s'\n", pname); return 1; }
    static char env;

    // 1) Deserialize the picked-up sensor into a generic (as set_zero_psi does).
    FlipperFormat* keyff = load_file(keypath);
    if(!keyff) { printf("cannot load %s\n", keypath); return 1; }
    TPMSBlockGeneric g = {0};
    g.protocol_name = pname;
    if(tpms_block_generic_deserialize(&g, keyff) != SubGhzProtocolStatusOk) {
        printf("generic deserialize FAILED\n"); return 1;
    }
    printf("orig: id=0x%08lX bit=%u pressure=%.2f temp=%.0f\n",
           (unsigned long)g.id, g.data_count_bit, (double)g.pressure, (double)g.temperature);

    // 2) Zero the pressure and re-pack per protocol (the flat-tire step).
    g.protocol_name = pname;
    g.pressure = 0.0f;
    if(!strcmp(pname, "Jeep TPMS")) tpms_protocol_jeep_pack(&g);
    else if(!strcmp(pname, "Citroen TPMS")) tpms_protocol_citroen_pack(&g);
    else { printf("no packer wired for '%s' in this test\n", pname); return 1; }
    printf("packed: id=0x%08lX bit=%u data=0x%016llX\n",
           (unsigned long)g.id, g.data_count_bit, (unsigned long long)g.data);

    // 3) Re-serialize (replace_tpms_payload) with a fake TPMS custom preset.
    SubGhzRadioPreset preset = {0};
    preset.name = furi_string_alloc_set("TPMS");
    preset.frequency = 433920000;
    preset.data = NULL; preset.data_size = 0;
    FlipperFormat* fresh = flipper_format_string_alloc();
    if(tpms_block_generic_serialize(&g, fresh, &preset) != SubGhzProtocolStatusOk) {
        printf("serialize FAILED\n"); return 1;
    }

    // 4) Encoder deserialize (the tx_start path) + collect yielded edges.
    void* enc = ep->encoder->alloc((SubGhzEnvironment*)&env);
    flipper_format_rewind(fresh);
    SubGhzProtocolStatus st = ep->encoder->deserialize(enc, fresh);
    printf("encoder deserialize: %d (%s)\n", (int)st, st == SubGhzProtocolStatusOk ? "OK" : "ERR");
    if(st != SubGhzProtocolStatusOk) return 1;
    static int raw[4096]; size_t n = 0;
    for(int i = 0; i < 2000 && n < 4090; i++) {
        LevelDuration ld = ep->encoder->yield(enc);
        if(level_duration_is_reset(ld)) break;
        uint32_t d = level_duration_get_duration(ld);
        bool lv = level_duration_get_level(ld);
        if(d == 0) break;
        raw[n++] = lv ? (int)d : -(int)d;
    }
    printf("encoder produced %zu edges\n", n);

    // 5) Feed all decoders -> confirm the 0-PSI frame decodes and reports ~0 bar.
    int any = 0;
    for(size_t i = 0; i < NDEC; i++) {
        const SubGhzProtocol* p = ALLDEC[i];
        if(!p->decoder || !p->decoder->alloc) continue;
        void* dec = p->decoder->alloc((SubGhzEnvironment*)&env);
        SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
        if(p->decoder->reset) p->decoder->reset(dec);
        HitFlag hf = {0};
        subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
        for(size_t s = 0; s < n; s++) p->decoder->feed(dec, raw[s] > 0, (uint32_t)(raw[s] < 0 ? -raw[s] : raw[s]));
        if(hf.hit) {
            FuriString* out = furi_string_alloc();
            if(p->decoder->get_string) p->decoder->get_string(dec, out);
            char fb[512]; flat(furi_string_get_cstr(out), fb, sizeof(fb));
            printf("  DECODED by %s\tHITS=%d\t%s\n", p->name, hf.hit, fb);
            furi_string_free(out); any = 1;
        }
        if(p->decoder->free) p->decoder->free(dec);
    }
    printf(any ? "RESULT: 0-PSI frame IS decodable\n" : "RESULT: 0-PSI frame NOT decodable\n");
    return any ? 0 : 3;
}
