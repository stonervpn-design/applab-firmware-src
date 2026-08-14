// Host decode/encode harness driving the REAL firmware lib/subghz automotive
// decoders -- the SAME decoder set the DEVICE registers in
// lib/subghz/protocols/protocol_items.c (subghz_protocol_registry_items_automotive[],
// #ifndef PUBLIC_BUILD), minus subghz_protocol_raw/subghz_protocol_bin_raw.
//
// This is a SEPARATE binary from pp_decode (which tests only the 27 proto_pirate
// copies). It exists so the harness reports the FULL device automotive coverage.
// Output format + main()/keystore-env setup are IDENTICAL to pp_decode.c; only
// the extern list + REGISTRY[] array differ (grown to the full lib set).
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

// forward: fresh-decode a duration buffer through one protocol, return get_string
static int feed_decode(
    const SubGhzProtocol* proto,
    const int* durs,
    size_t n,
    FuriString* out_str);

// ---- protocol registry (extern from the lib/subghz/protocols decoder .c files) ----
// Symbol per file confirmed via `grep "const SubGhzProtocol <sym> =" lib/subghz/protocols/<f>.c`.
extern const SubGhzProtocol bmw_cas4_protocol;
extern const SubGhzProtocol hyundai_kia_rio_protocol;
extern const SubGhzProtocol hyundai_tl_protocol;
extern const SubGhzProtocol mazda_siemens_protocol;
extern const SubGhzProtocol sheriff_cfm_protocol;
extern const SubGhzProtocol fiat_v1_protocol;
extern const SubGhzProtocol chrysler_protocol_v0;
extern const SubGhzProtocol fiat_protocol_v0;
extern const SubGhzProtocol ford_protocol_v0;
extern const SubGhzProtocol ford_protocol_v1;
extern const SubGhzProtocol ford_protocol_v2;
extern const SubGhzProtocol ford_protocol_v3;
extern const SubGhzProtocol honda_static_protocol;
extern const SubGhzProtocol kia_protocol_v0;
extern const SubGhzProtocol kia_protocol_v1;
extern const SubGhzProtocol kia_protocol_v2;
extern const SubGhzProtocol kia_protocol_v3_v4;
extern const SubGhzProtocol kia_protocol_v5;
extern const SubGhzProtocol kia_protocol_v6;
extern const SubGhzProtocol kia_protocol_v7;
extern const SubGhzProtocol land_rover_v0_protocol;
extern const SubGhzProtocol land_rover_rke_protocol;
extern const SubGhzProtocol mazda_v0_protocol;
extern const SubGhzProtocol mitsubishi_v0_protocol;
extern const SubGhzProtocol porsche_touareg_protocol;
extern const SubGhzProtocol psa_protocol;
extern const SubGhzProtocol subghz_protocol_scher_khan;
extern const SubGhzProtocol subghz_protocol_star_line;
extern const SubGhzProtocol subaru_protocol;
extern const SubGhzProtocol vag_protocol;
extern const SubGhzProtocol subghz_protocol_audi_v1;
extern const SubGhzProtocol subghz_protocol_audi_v2;
extern const SubGhzProtocol subghz_protocol_audi_v3;
extern const SubGhzProtocol subghz_protocol_bmw_v2;
extern const SubGhzProtocol subghz_protocol_bmw_v3;
extern const SubGhzProtocol subghz_protocol_chevrolet_v0;
extern const SubGhzProtocol subghz_protocol_ducato_v0;
extern const SubGhzProtocol subghz_protocol_fiat_spa;
extern const SubGhzProtocol subghz_protocol_fiat_v2_v3_v5;
extern const SubGhzProtocol subghz_protocol_hitag2;
extern const SubGhzProtocol subghz_protocol_honda_v2_v3;
extern const SubGhzProtocol subghz_protocol_honda_v4;
extern const SubGhzProtocol subghz_protocol_honda_v5;
extern const SubGhzProtocol subghz_protocol_honda_v6;
extern const SubGhzProtocol subghz_protocol_honda_v7;
extern const SubGhzProtocol subghz_protocol_honda_v8;
extern const SubGhzProtocol subghz_protocol_honda_v9;
extern const SubGhzProtocol subghz_protocol_honda_v10;
extern const SubGhzProtocol subghz_protocol_jeep_v0;
extern const SubGhzProtocol subghz_protocol_ldv_v0;
extern const SubGhzProtocol subghz_protocol_lexus_v0;
extern const SubGhzProtocol subghz_protocol_mazda_v1;
extern const SubGhzProtocol subghz_protocol_mazda_v2;
extern const SubGhzProtocol subghz_protocol_mitsubishi_v1;
extern const SubGhzProtocol subghz_protocol_nissan_v0;
extern const SubGhzProtocol subghz_protocol_opel_v0;
extern const SubGhzProtocol subghz_protocol_peugot_v0;
extern const SubGhzProtocol subghz_protocol_renault_v1;
extern const SubGhzProtocol subghz_protocol_renault_v2;
extern const SubGhzProtocol subghz_protocol_renault_v3;
extern const SubGhzProtocol subghz_protocol_renault_v4;
extern const SubGhzProtocol subghz_protocol_toyota_v0;
extern const SubGhzProtocol toyota_keeloq_protocol;

// EXACTLY the &... entries of protocol_items.c's automotive array (in file order),
// minus subghz_protocol_raw / subghz_protocol_bin_raw.
static const SubGhzProtocol* const REGISTRY[] = {
    // former group A
    &bmw_cas4_protocol,          &hyundai_kia_rio_protocol,
    &hyundai_tl_protocol,        &mazda_siemens_protocol,
    &sheriff_cfm_protocol,       &fiat_v1_protocol,
    &chrysler_protocol_v0,       &fiat_protocol_v0,
    &ford_protocol_v0,           &ford_protocol_v1,
    &ford_protocol_v2,           &ford_protocol_v3,
    &honda_static_protocol,      &kia_protocol_v0,
    &kia_protocol_v1,            &kia_protocol_v2,
    &kia_protocol_v3_v4,         &kia_protocol_v5,
    &kia_protocol_v6,            &kia_protocol_v7,
    &land_rover_v0_protocol,     &land_rover_rke_protocol,
    &mazda_v0_protocol,
    &mitsubishi_v0_protocol,     &porsche_touareg_protocol,
    &psa_protocol,               &subghz_protocol_scher_khan,
    &subghz_protocol_star_line,  &subaru_protocol,
    &vag_protocol,               &subghz_protocol_audi_v1,
    &subghz_protocol_audi_v2,
    // former group B
    &subghz_protocol_audi_v3,        &subghz_protocol_bmw_v2,
    &subghz_protocol_bmw_v3,         &subghz_protocol_chevrolet_v0,
    &subghz_protocol_ducato_v0,      &subghz_protocol_fiat_spa,
    &subghz_protocol_fiat_v2_v3_v5,  &subghz_protocol_hitag2,
    &subghz_protocol_honda_v2_v3,    &subghz_protocol_honda_v4,
    &subghz_protocol_honda_v5,       &subghz_protocol_honda_v6,
    &subghz_protocol_honda_v7,       &subghz_protocol_honda_v8,
    &subghz_protocol_honda_v9,       &subghz_protocol_honda_v10,
    &subghz_protocol_jeep_v0,        &subghz_protocol_ldv_v0,
    &subghz_protocol_lexus_v0,       &subghz_protocol_mazda_v1,
    &subghz_protocol_mazda_v2,       &subghz_protocol_mitsubishi_v1,
    &subghz_protocol_nissan_v0,      &subghz_protocol_opel_v0,
    &subghz_protocol_peugot_v0,      &subghz_protocol_renault_v1,
    &subghz_protocol_renault_v2,     &subghz_protocol_renault_v3,
    &subghz_protocol_renault_v4,     &subghz_protocol_toyota_v0,
    &toyota_keeloq_protocol,
};
#define REG_N (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

// ---- RAW parse ----
// Reads every "RAW_Data:" line, concatenating the signed durations.
static int* load_raw(const char* path, size_t* out_n) {
    FILE* f = fopen(path, "rb");
    if(getenv("PPDBG")) fprintf(stderr, "[dbg] fopen(%s)=%p\n", path, (void*)f);
    if(!f) return NULL;
    int rawlines = 0;
    size_t cap = 4096, n = 0;
    int* buf = malloc(cap * sizeof(int));
    char line[8192];
    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "RAW_Data:", 9) != 0) continue;
        rawlines++;
        char* p = line + 9;
        char* tok = strtok(p, " \t\r\n");
        while(tok) {
            if(*tok == '-' || (*tok >= '0' && *tok <= '9')) {
                if(n == cap) {
                    cap *= 2;
                    buf = realloc(buf, cap * sizeof(int));
                }
                buf[n++] = atoi(tok);
            }
            tok = strtok(NULL, " \t\r\n");
        }
    }
    fclose(f);
    if(getenv("PPDBG")) fprintf(stderr, "[dbg] rawlines=%d parsed=%zu\n", rawlines, n);
    *out_n = n;
    return buf;
}

// ---- decode callback ----
typedef struct {
    int hit;
} HitFlag;

static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

static void flatten(const char* c, char* out, size_t cap) {
    size_t j = 0;
    for(; c && *c && j < cap - 1; c++) out[j++] = (*c == '\n') ? '|' : *c;
    out[j] = 0;
}

// Fresh-decode a duration buffer through one protocol; fill out_str with get_string.
static int feed_decode(const SubGhzProtocol* proto, const int* durs, size_t n, FuriString* out_str) {
    static char dummy_env2;
    void* dec = proto->decoder->alloc((SubGhzEnvironment*)&dummy_env2);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
    for(size_t s = 0; s < n; s++) {
        int v = durs[s];
        proto->decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    if(hf.hit && proto->decoder->get_string) proto->decoder->get_string(dec, out_str);
    if(proto->decoder->free) proto->decoder->free(dec);
    return hf.hit;
}

// Serialize a decoded key, run it through the encoder, re-decode the TX stream,
// and classify: EXACT (identical string), REDECODE (re-decodes, differs — normal
// for rolling counters), or a FAIL stage.
static const char* roundtrip(const SubGhzProtocol* proto, void* dec, const char* orig_flat) {
    static char dummy_env3;
    FuriString* pn = furi_string_alloc();
    furi_string_cat_str(pn, "AM650");
    SubGhzRadioPreset preset = {.name = pn, .frequency = 433920000, .data = NULL, .data_size = 0};
    FlipperFormat* ff = flipper_format_string_alloc();
    const char* rt = "RT:FAIL-ser";

    if(proto->decoder->serialize &&
       proto->decoder->serialize(dec, ff, &preset) == SubGhzProtocolStatusOk) {
        flipper_format_rewind(ff);
        void* enc = proto->encoder->alloc((SubGhzEnvironment*)&dummy_env3);
        if(proto->encoder->deserialize(enc, ff) == SubGhzProtocolStatusOk) {
            static int edurs[16384];
            size_t en = 0;
            for(int guard = 0; guard < 16384; guard++) {
                LevelDuration ld = proto->encoder->yield(enc);
                if(level_duration_is_reset(ld)) break;
                if(en < 16384)
                    edurs[en++] = level_duration_get_level(ld) ?
                                      (int)level_duration_get_duration(ld) :
                                      -(int)level_duration_get_duration(ld);
            }
            if(en > 0) {
                FuriString* rs = furi_string_alloc();
                if(feed_decode(proto, edurs, en, rs)) {
                    char rflat[512];
                    flatten(furi_string_get_cstr(rs), rflat, sizeof(rflat));
                    rt = (strcmp(orig_flat, rflat) == 0) ? "RT:EXACT" : "RT:REDECODE";
                } else {
                    rt = "RT:FAIL-redecode";
                }
                furi_string_free(rs);
            } else {
                rt = "RT:FAIL-noupload";
            }
        } else {
            rt = "RT:FAIL-deser";
        }
        if(proto->encoder->free) proto->encoder->free(enc);
    }
    flipper_format_free(ff);
    furi_string_free(pn);
    return rt;
}

int main(int argc, char** argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: %s <capture.sub>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    size_t n = 0;
    int* raw = load_raw(path, &n);
    if(!raw || n == 0) {
        printf("%s\tNO_RAW\n", path);
        return 1;
    }
    if(getenv("PPDBG")) fprintf(stderr, "[dbg] n=%zu first=%d last=%d\n", n, raw[0], raw[n - 1]);

    for(size_t i = 0; i < REG_N; i++) {
        const SubGhzProtocol* proto = REGISTRY[i];
        if(!proto->decoder || !proto->decoder->alloc) continue;
        // non-NULL dummy environment: keeloq decoders bail on NULL; the host
        // keystore shim ignores the pointer and hands back an empty keystore.
        static char dummy_env;
        void* dec = proto->decoder->alloc((SubGhzEnvironment*)&dummy_env);
        SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
        HitFlag hf = {0};
        subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);

        for(size_t s = 0; s < n; s++) {
            int v = raw[s];
            bool level = v > 0;
            uint32_t dur = (uint32_t)(v < 0 ? -v : v);
            proto->decoder->feed(dec, level, dur);
        }

        if(hf.hit) {
            FuriString* out = furi_string_alloc();
            if(proto->decoder->get_string) proto->decoder->get_string(dec, out);
            char flat[512];
            flatten(furi_string_get_cstr(out), flat, sizeof(flat));

            // ---- encoder round-trip: serialize key -> encode -> re-decode ----
            const char* rt = "RT:no-encoder";
            if(proto->encoder && proto->encoder->alloc && proto->encoder->deserialize &&
               proto->encoder->yield) {
                rt = roundtrip(proto, dec, flat);
            }

            printf("%s\tDECODED\t%s\thits=%d\t%s\t%s\n", path, proto->name, hf.hit, rt, flat);
            furi_string_free(out);
        }
        if(proto->decoder->free) proto->decoder->free(dec);
    }
    free(raw);
    return 0;
}
