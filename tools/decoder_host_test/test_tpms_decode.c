/*
 * test_tpms_decode.c — host validation of the on-device TPMS decoders.
 *
 * Feeds the pulse-duration train from a Flipper RAW .sub into EVERY TPMS decoder
 * (the same const SubGhzProtocol structs the firmware registers) exactly as the
 * device's SubGhz worker would: decoder->feed(dec, level, duration) per edge.
 * Prints which decoder(s) fire + their get_string output.
 *
 * Purpose: prove the DECODE LOGIC is sound independent of RF / device config, so
 * we know the TPMS-spoofer "nothing decodes" issue is the RX preset/freq, not the
 * decoders. Best fed the real Flipper Ford capture (Ford_TPMS-good.sub).
 *
 * Build:  bash tools/decoder_host_test/build_test_tpms.sh
 * Run:    ./tools/decoder_host_test/test_tpms_decode <capture.sub> [more.sub ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/types.h>
#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>

/* ---- the 17 TPMS decoders the firmware registers ---- */
extern const SubGhzProtocol tpms_protocol_ford;
extern const SubGhzProtocol tpms_protocol_toyota;
extern const SubGhzProtocol tpms_protocol_pmv107j;
extern const SubGhzProtocol tpms_protocol_honda;
extern const SubGhzProtocol tpms_protocol_nissan;
extern const SubGhzProtocol tpms_protocol_hyundai_vdo;
extern const SubGhzProtocol tpms_protocol_hyundai_ev;
extern const SubGhzProtocol tpms_protocol_renault;
extern const SubGhzProtocol tpms_protocol_citroen;
extern const SubGhzProtocol tpms_protocol_abarth124;
extern const SubGhzProtocol tpms_protocol_jansite;
extern const SubGhzProtocol tpms_protocol_jeep;
extern const SubGhzProtocol tpms_protocol_truck;
extern const SubGhzProtocol tpms_protocol_porsche;
extern const SubGhzProtocol tpms_protocol_bmw;
extern const SubGhzProtocol tpms_protocol_bmw_g3;
extern const SubGhzProtocol tpms_protocol_schrader_eg53ma4;
extern const SubGhzProtocol tpms_protocol_schrader_gg4;

static const SubGhzProtocol* const REGISTRY[] = {
    &tpms_protocol_ford,       &tpms_protocol_toyota,   &tpms_protocol_pmv107j,
    &tpms_protocol_honda,      &tpms_protocol_nissan,   &tpms_protocol_hyundai_vdo,
    &tpms_protocol_hyundai_ev,
    &tpms_protocol_renault,    &tpms_protocol_citroen,  &tpms_protocol_abarth124,
    &tpms_protocol_jansite,    &tpms_protocol_jeep,     &tpms_protocol_truck,
    &tpms_protocol_porsche,    &tpms_protocol_bmw,      &tpms_protocol_bmw_g3,
    &tpms_protocol_schrader_eg53ma4, &tpms_protocol_schrader_gg4,
};
#define REG_N (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

/* ---- .sub RAW_Data parser (verbatim from pp_decode_full) ---- */
static int* load_raw(const char* path, size_t* out_n) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    size_t cap = 4096, n = 0;
    int* buf = malloc(cap * sizeof(int));
    char line[8192];
    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "RAW_Data:", 9) != 0) continue;
        char* tok = strtok(line + 9, " \t\r\n");
        while(tok) {
            if(*tok == '-' || (*tok >= '0' && *tok <= '9')) {
                if(n == cap) { cap *= 2; buf = realloc(buf, cap * sizeof(int)); }
                buf[n++] = atoi(tok);
            }
            tok = strtok(NULL, " \t\r\n");
        }
    }
    fclose(f);
    *out_n = n;
    return buf;
}

typedef struct { int hit; } HitFlag;
static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

static void flatten(const char* c, char* out, size_t cap) {
    size_t j = 0;
    for(size_t i = 0; c[i] && j + 1 < cap; i++)
        out[j++] = (c[i] == '\r' || c[i] == '\n') ? ' ' : c[i];
    out[j] = 0;
}

int main(int argc, char** argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: %s <capture.sub> [more.sub ...]\n", argv[0]);
        return 2;
    }
    int any_overall = 0;
    for(int ai = 1; ai < argc; ai++) {
        const char* path = argv[ai];
        size_t n = 0;
        int* raw = load_raw(path, &n);
        if(!raw || n == 0) {
            printf("%s\tNO_RAW\n", path);
            free(raw);
            continue;
        }
        int any = 0;
        for(size_t i = 0; i < REG_N; i++) {
            const SubGhzProtocol* proto = REGISTRY[i];
            if(!proto->decoder || !proto->decoder->alloc) continue;
            static char dummy_env;
            void* dec = proto->decoder->alloc((SubGhzEnvironment*)&dummy_env);
            SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
            if(proto->decoder->reset) proto->decoder->reset(dec);
            HitFlag hf = {0};
            subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
            for(size_t s = 0; s < n; s++) {
                int v = raw[s];
                proto->decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
            }
            if(hf.hit) {
                FuriString* out = furi_string_alloc();
                if(proto->decoder->get_string) proto->decoder->get_string(dec, out);
                char flat[512];
                flatten(furi_string_get_cstr(out), flat, sizeof(flat));
                printf("%s\t%s\tHITS=%d\t%s\n", path, proto->name, hf.hit, flat);
                furi_string_free(out);
                any = 1; any_overall = 1;
            }
            if(proto->decoder->free) proto->decoder->free(dec);
        }
        if(!any) printf("%s\t(no TPMS decoder fired; %zu edges)\n", path, n);
        free(raw);
    }
    printf("== TPMS host decode: %s ==\n", any_overall ? "at least one decoder FIRED" : "NOTHING decoded");
    return 0;
}
