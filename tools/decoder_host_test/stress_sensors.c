/*
 * stress_sensors.c — anti-phantom host validation of the SENSORS registry.
 *
 * Feeds a Flipper RAW .sub pulse train into EVERY sensors-mode decoder that is
 * NOT already covered by test_tpms_decode: the 8 rtl_433 weather decoders and
 * the 22 extended TPMS/Schrader variants. Same feed model as test_tpms_decode
 * (decoder->feed(dec, level, duration) per edge) + get_string print on a hit.
 *
 * Purpose: prove noise / degenerate PWM/PPM captures do NOT produce phantom
 * decodes. Any HIT line on a stress/noise file is a phantom to be gated.
 *
 * Build:  bash tools/decoder_host_test/build_stress_sensors.sh
 * Run:    ./tools/decoder_host_test/stress_sensors <capture.sub> [more.sub ...]
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

/* ---- 8 rtl_433 weather decoders ---- */
extern const SubGhzProtocol subghz_protocol_acurite_tower;
extern const SubGhzProtocol subghz_protocol_lacrosse_tx141th;
extern const SubGhzProtocol subghz_protocol_lacrosse_tx;
extern const SubGhzProtocol subghz_protocol_nexus;
extern const SubGhzProtocol subghz_protocol_prologue;
extern const SubGhzProtocol subghz_protocol_oregon_thgr;
extern const SubGhzProtocol subghz_protocol_fineoffset_wh2;
extern const SubGhzProtocol subghz_protocol_ambient_weather;

/* ---- 22 extended TPMS / Schrader variants NOT in test_tpms_decode ---- */
extern const SubGhzProtocol tpms_protocol_tpms_elantra2012;
extern const SubGhzProtocol tpms_protocol_tpms_ave;
extern const SubGhzProtocol tpms_protocol_tpms_renault_0435r;
extern const SubGhzProtocol tpms_protocol_tpms_kia;
extern const SubGhzProtocol tpms_protocol_tpms_trw_fsk;
extern const SubGhzProtocol tpms_protocol_tpms_trw_ook;
extern const SubGhzProtocol tpms_protocol_tpms_smartire;
extern const SubGhzProtocol tpms_protocol_tpms_jansite_ty468;
extern const SubGhzProtocol tpms_protocol_tpms_jansite_ty588;
extern const SubGhzProtocol tpms_protocol_tpms_mercedes_benz;
extern const SubGhzProtocol tpms_protocol_tpms_airpuxem;
extern const SubGhzProtocol tpms_protocol_tpms_tyreguard400;
extern const SubGhzProtocol tpms_protocol_tpms_gm;
extern const SubGhzProtocol tpms_protocol_tpms_eezrv;
extern const SubGhzProtocol tpms_protocol_tpms_schrader_motorcycle;
extern const SubGhzProtocol tpms_protocol_tpms_imars_t240;
extern const SubGhzProtocol tpms_protocol_tpms_sefis_m3;
extern const SubGhzProtocol tpms_protocol_tpms_schraeder;
extern const SubGhzProtocol tpms_protocol_schrader_smd3ma4;
extern const SubGhzProtocol tpms_protocol_schrader_nis315g3;
extern const SubGhzProtocol tpms_protocol_schrader_gg4;
extern const SubGhzProtocol tpms_protocol_schrader_eg53ma4;

static const SubGhzProtocol* const REGISTRY[] = {
    &subghz_protocol_acurite_tower,       &subghz_protocol_lacrosse_tx141th,
    &subghz_protocol_lacrosse_tx,         &subghz_protocol_nexus,
    &subghz_protocol_prologue,            &subghz_protocol_oregon_thgr,
    &subghz_protocol_fineoffset_wh2,      &subghz_protocol_ambient_weather,
    &tpms_protocol_tpms_elantra2012,      &tpms_protocol_tpms_ave,
    &tpms_protocol_tpms_renault_0435r,    &tpms_protocol_tpms_kia,
    &tpms_protocol_tpms_trw_fsk,          &tpms_protocol_tpms_trw_ook,
    &tpms_protocol_tpms_smartire,         &tpms_protocol_tpms_jansite_ty468,
    &tpms_protocol_tpms_jansite_ty588,    &tpms_protocol_tpms_mercedes_benz,
    &tpms_protocol_tpms_airpuxem,         &tpms_protocol_tpms_tyreguard400,
    &tpms_protocol_tpms_gm,               &tpms_protocol_tpms_eezrv,
    &tpms_protocol_tpms_schrader_motorcycle, &tpms_protocol_tpms_imars_t240,
    &tpms_protocol_tpms_sefis_m3,         &tpms_protocol_tpms_schraeder,
    &tpms_protocol_schrader_smd3ma4,      &tpms_protocol_schrader_nis315g3,
    &tpms_protocol_schrader_gg4,          &tpms_protocol_schrader_eg53ma4,
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
    int total_phantoms = 0;
    for(int ai = 1; ai < argc; ai++) {
        const char* path = argv[ai];
        size_t n = 0;
        int* raw = load_raw(path, &n);
        if(!raw || n == 0) {
            printf("%s\tNO_RAW\n", path);
            free(raw);
            continue;
        }
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
                printf("PHANTOM\t%s\t%s\tHITS=%d\t%s\n",
                       path, proto->name, hf.hit, flat);
                furi_string_free(out);
                total_phantoms += hf.hit;
            }
            if(proto->decoder->free) proto->decoder->free(dec);
        }
        free(raw);
    }
    printf("== SENSORS stress: %d phantom decode(s) ==\n", total_phantoms);
    return total_phantoms ? 1 : 0;
}
