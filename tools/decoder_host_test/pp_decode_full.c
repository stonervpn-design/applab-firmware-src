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
extern const SubGhzProtocol bmw_ook_protocol;
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
extern const SubGhzProtocol subghz_protocol_compustar;
extern const SubGhzProtocol subghz_protocol_astrostart;
extern const SubGhzProtocol subghz_protocol_audiovox;
extern const SubGhzProtocol subghz_protocol_gm_rke;
extern const SubGhzProtocol subghz_protocol_continental_rke;
extern const SubGhzProtocol subghz_protocol_nidec_rke;
extern const SubGhzProtocol subghz_protocol_alps_rke;
extern const SubGhzProtocol vag_polo_protocol;
extern const SubGhzProtocol subghz_protocol_retekess;

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
    &bmw_ook_protocol,
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
    // grafted aftermarket remote-start + OEM RKE (decode-only)
    &subghz_protocol_compustar,      &subghz_protocol_astrostart,
    &subghz_protocol_audiovox,       &subghz_protocol_gm_rke,
    &subghz_protocol_continental_rke, &subghz_protocol_nidec_rke,
    &subghz_protocol_alps_rke,       &vag_polo_protocol,
    &subghz_protocol_retekess,
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

static void flatten(const char* c, char* out, size_t cap);
// FRAMEDUMP: on every decoded frame, print its get_string so we can see the
// per-press counter SEQUENCE inside a multi-press capture (repeats vs rolling).
static const SubGhzProtocol* g_fd_proto;
static void* g_fd_dec;
static void rx_cb_dump(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
    if(g_fd_proto && g_fd_dec && g_fd_proto->decoder->get_string) {
        FuriString* s = furi_string_alloc();
        g_fd_proto->decoder->get_string(g_fd_dec, s);
        char flat[512];
        flatten(furi_string_get_cstr(s), flat, sizeof(flat));
        fprintf(stderr, "  [frame %d] %s\n", ((HitFlag*)ctx)->hit, flat);
        furi_string_free(s);
    }
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
            // Physical-RF fidelity: a real TX/RX only sees level TRANSITIONS, so
            // two consecutive same-level LevelDuration entries physically MERGE
            // into a single pulse. Coalesce same-sign runs before re-decoding so
            // the round-trip catches encoders that emit adjacent same-level pulses
            // (which the decoder tolerates when fed the raw list, but which corrupt
            // the on-air waveform). A correct clone must survive this merge.
            {
                size_t mn = 0;
                for(size_t k = 0; k < en; k++) {
                    if(mn > 0 && ((edurs[k] < 0) == (edurs[mn - 1] < 0))) {
                        int sign = edurs[mn - 1] < 0 ? -1 : 1;
                        long mag = (long)abs(edurs[mn - 1]) + (long)abs(edurs[k]);
                        if(mag > 2000000000L) mag = 2000000000L;
                        edurs[mn - 1] = sign * (int)mag;
                    } else {
                        edurs[mn++] = edurs[k];
                    }
                }
                en = mn;
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

// Yield one frame from a fresh encoder deserialized from ff (at its start).
static void synth_yield(
    const SubGhzProtocol* proto,
    FlipperFormat* ff,
    int* buf,
    size_t* pn,
    size_t cap) {
    static char dummy_env4;
    *pn = 0;
    flipper_format_rewind(ff);
    void* enc = proto->encoder->alloc((SubGhzEnvironment*)&dummy_env4);
    if(proto->encoder->deserialize(enc, ff) == SubGhzProtocolStatusOk) {
        for(int g = 0; g < 16384; g++) {
            LevelDuration ld = proto->encoder->yield(enc);
            if(level_duration_is_reset(ld)) break;
            if(*pn < cap)
                buf[(*pn)++] = level_duration_get_level(ld) ?
                                   (int)level_duration_get_duration(ld) :
                                   -(int)level_duration_get_duration(ld);
        }
    }
    if(proto->encoder->free) proto->encoder->free(enc);
}

// Second-fob synthesis check: does bumping the counter produce a DIFFERENT,
// still-decodable frame (true rolling synth) or the identical frame (clone-only,
// single-use)? Uses the standard "Cnt" field.
static const char* synthcheck(const SubGhzProtocol* proto, void* dec) {
    FuriString* pn = furi_string_alloc();
    furi_string_cat_str(pn, "AM650");
    SubGhzRadioPreset preset = {
        .name = pn, .frequency = 433920000, .data = NULL, .data_size = 0};
    FlipperFormat* ff = flipper_format_string_alloc();
    const char* r = "SYNTH:fail-ser";
    if(proto->decoder->serialize &&
       proto->decoder->serialize(dec, ff, &preset) == SubGhzProtocolStatusOk) {
        // The counter field is protocol-specific (see subghz_fob_counter_field):
        // most use "Cnt"; some use "Rolling"/"RollingCode"/"Counter".
        static const char* CF[] = {"Cnt", "Rolling", "RollingCode", "Counter"};
        const char* cfield = NULL;
        uint32_t c0 = 0;
        for(unsigned i = 0; i < sizeof(CF) / sizeof(CF[0]); i++) {
            flipper_format_rewind(ff);
            if(flipper_format_read_uint32(ff, CF[i], &c0, 1)) {
                cfield = CF[i];
                break;
            }
        }
        if(!cfield) {
            r = "SYNTH:no-cnt";
        } else {
            static int fa[16384], fb[16384];
            size_t na = 0, nb = 0;
            synth_yield(proto, ff, fa, &na, 16384);
            uint32_t c1 = c0 + 1;
            flipper_format_rewind(ff);
            flipper_format_update_uint32(ff, cfield, &c1, 1);
            synth_yield(proto, ff, fb, &nb, 16384);
            if(na == 0 || nb == 0) {
                r = "SYNTH:no-upload";
            } else if(na == nb && memcmp(fa, fb, na * sizeof(int)) == 0) {
                r = "SYNTH:clone-only";
            } else {
                FuriString* rs = furi_string_alloc();
                r = feed_decode(proto, fb, nb, rs) ? "SYNTH:ok" : "SYNTH:badframe";
                furi_string_free(rs);
            }
        }
    }
    flipper_format_free(ff);
    furi_string_free(pn);
    return r;
}

// Extract the token value after a "Label:" marker in a flattened get_string.
static void extract_after(const char* flat, const char* label, char* out, size_t cap) {
    out[0] = 0;
    const char* p = strstr(flat, label);
    if(!p) return;
    p += strlen(label);
    size_t j = 0;
    while(*p && *p != ' ' && *p != '|' && j < cap - 1) out[j++] = *p++;
    out[j] = 0;
}

// Decode a duration buffer through one protocol; return hit count and, on a hit,
// fill the flattened get_string and read back the rolling counter via serialize.
static int decode_probe(
    const SubGhzProtocol* proto, const int* durs, size_t nn,
    char* flat, size_t flatcap, uint32_t* cnt_out, int* has_cnt) {
    static char de;
    void* dec = proto->decoder->alloc((SubGhzEnvironment*)&de);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
    for(size_t s = 0; s < nn; s++) {
        int v = durs[s];
        proto->decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    flat[0] = 0;
    *has_cnt = 0;
    if(hf.hit) {
        FuriString* out = furi_string_alloc();
        if(proto->decoder->get_string) proto->decoder->get_string(dec, out);
        flatten(furi_string_get_cstr(out), flat, flatcap);
        furi_string_free(out);
        if(proto->decoder->serialize) {
            FuriString* pn = furi_string_alloc();
            furi_string_cat_str(pn, "AM650");
            SubGhzRadioPreset preset = {
                .name = pn, .frequency = 433920000, .data = NULL, .data_size = 0};
            FlipperFormat* ff = flipper_format_string_alloc();
            if(proto->decoder->serialize(dec, ff, &preset) == SubGhzProtocolStatusOk) {
                static const char* CF[] = {"Cnt", "Rolling", "RollingCode", "Counter"};
                for(unsigned i = 0; i < 4; i++) {
                    flipper_format_rewind(ff);
                    if(flipper_format_read_uint32(ff, CF[i], cnt_out, 1)) {
                        *has_cnt = 1;
                        break;
                    }
                }
            }
            flipper_format_free(ff);
            furi_string_free(pn);
        }
    }
    int h = hf.hit;
    if(proto->decoder->free) proto->decoder->free(dec);
    return h;
}

// MONOTONIC LOCKSTEP CHECK (desync-safety proof): for a decoded capture at counter
// c0, synthesize frames at c0+1..c0+5 and verify each (a) re-decodes, (b) carries
// EXACTLY the requested counter, (c) keeps the SAME serial (Sn) — identity
// preserved, and (d) all frames are pairwise DISTINCT. If all hold, the synthesized
// 2nd fob emits precisely the codes the real fob would at those counters => perfect
// lockstep => no avoidable desync of the original.
static void synthmono(const SubGhzProtocol* proto, void* dec, char* outbuf, size_t outcap) {
    FuriString* pn = furi_string_alloc();
    furi_string_cat_str(pn, "AM650");
    SubGhzRadioPreset preset = {
        .name = pn, .frequency = 433920000, .data = NULL, .data_size = 0};
    FlipperFormat* ff = flipper_format_string_alloc();
    snprintf(outbuf, outcap, "\tMONO:fail-ser");
    if(!(proto->decoder->serialize &&
         proto->decoder->serialize(dec, ff, &preset) == SubGhzProtocolStatusOk))
        goto done;
    // Pick the LOAD-BEARING counter field: the CF[] field that is present AND whose
    // +1 bump actually changes the yielded frame. (Some protocols carry a vestigial
    // "Cnt" plus the real rolling field, e.g. HondaV8 uses "RollingCode".)
    static const char* CF[] = {"Cnt", "Rolling", "RollingCode", "Counter"};
    const char* cf = NULL;
    uint32_t c0 = 0;
    static int probeA[16384], probeB[16384];
    for(unsigned i = 0; i < 4; i++) {
        uint32_t v = 0;
        flipper_format_rewind(ff);
        if(!flipper_format_read_uint32(ff, CF[i], &v, 1)) continue;
        size_t na = 0, nb = 0;
        uint32_t v1 = v;
        flipper_format_rewind(ff);
        flipper_format_update_uint32(ff, CF[i], &v1, 1);
        synth_yield(proto, ff, probeA, &na, 16384);
        uint32_t v2 = v + 1;
        flipper_format_rewind(ff);
        flipper_format_update_uint32(ff, CF[i], &v2, 1);
        synth_yield(proto, ff, probeB, &nb, 16384);
        // restore
        flipper_format_rewind(ff);
        flipper_format_update_uint32(ff, CF[i], &v, 1);
        if(na && nb && !(na == nb && memcmp(probeA, probeB, na * sizeof(int)) == 0)) {
            cf = CF[i];
            c0 = v;
            break;
        }
    }
    if(!cf) {
        snprintf(outbuf, outcap, "\tMONO:no-cnt/clone-only");
        goto done;
    }
    // Desync-safety invariant (offset-agnostic): synthesizing counter c0+0..c0+5
    // must give frames that (a) all re-decode, (b) have STRICTLY MONOTONIC decoded
    // counters with a CONSTANT step (the encoder may pre-increment/self-advance, so
    // absolute value need not equal the request — only clean per-press progression
    // matters), (c) keep the same Sn, (d) are pairwise distinct. That == the exact
    // ordered code sequence a real fob emits => the car's window only ever advances.
    static int frames[6][16384];
    size_t fn[6];
    char flats[6][512];
    uint32_t cks[6];
    int hasc[6];
    int ok = 1;
    char detail[128] = "";
    char sn0[64] = "";
    for(int k = 0; k <= 5 && ok; k++) {
        uint32_t ck = c0 + (uint32_t)k;
        flipper_format_rewind(ff);
        flipper_format_update_uint32(ff, cf, &ck, 1);
        synth_yield(proto, ff, frames[k], &fn[k], 16384);
        if(fn[k] == 0) {
            ok = 0;
            snprintf(detail, sizeof(detail), "noupload@+%d", k);
            break;
        }
        if(!decode_probe(proto, frames[k], fn[k], flats[k], 512, &cks[k], &hasc[k]) ||
           !hasc[k]) {
            ok = 0;
            snprintf(detail, sizeof(detail), "nodecode@+%d", k);
            break;
        }
        char snk[64];
        extract_after(flats[k], "Sn:", snk, sizeof(snk));
        if(k == 0) {
            snprintf(sn0, sizeof(sn0), "%s", snk);
        } else if(sn0[0] && strcmp(sn0, snk) != 0) {
            ok = 0;
            snprintf(detail, sizeof(detail), "Sn %s!=%s@+%d", snk, sn0, k);
            break;
        }
    }
    long step = 0;
    if(ok) {
        step = (long)cks[1] - (long)cks[0];
        for(int k = 1; k <= 5 && ok; k++) {
            long s = (long)cks[k] - (long)cks[k - 1];
            if(s <= 0 || s != step) {
                ok = 0;
                snprintf(
                    detail, sizeof(detail), "nonmono cnt=[%lu,%lu,%lu,%lu,%lu,%lu]",
                    (unsigned long)cks[0], (unsigned long)cks[1], (unsigned long)cks[2],
                    (unsigned long)cks[3], (unsigned long)cks[4], (unsigned long)cks[5]);
            }
        }
    }
    if(ok) {
        for(int a = 0; a < 6 && ok; a++)
            for(int b = a + 1; b < 6 && ok; b++)
                if(fn[a] == fn[b] &&
                   memcmp(frames[a], frames[b], fn[a] * sizeof(int)) == 0) {
                    ok = 0;
                    snprintf(detail, sizeof(detail), "dup@+%d,+%d", a, b);
                }
    }
    if(ok)
        snprintf(
            outbuf, outcap, "\tMONO:ok fld=%s step=%ld +5-lockstep Sn=%s", cf, step,
            sn0[0] ? sn0 : "?");
    else
        snprintf(outbuf, outcap, "\tMONO:FAIL(%s) %s", cf, detail);
done:
    flipper_format_free(ff);
    furi_string_free(pn);
}

int main(int argc, char** argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: %s <capture.sub> [more.sub ...]\n", argv[0]);
        return 2;
    }
    for(int ai = 1; ai < argc; ai++) {
    const char* path = argv[ai];
    size_t n = 0;
    int* raw = load_raw(path, &n);
    if(!raw || n == 0) {
        printf("%s\tNO_RAW\n", path);
        if(raw) free(raw);
        continue;
    }
    if(getenv("PPDBG")) fprintf(stderr, "[dbg] n=%zu first=%d last=%d\n", n, raw[0], raw[n - 1]);

    for(size_t i = 0; i < REG_N; i++) {
        const SubGhzProtocol* proto = REGISTRY[i];
        if(!proto->decoder || !proto->decoder->alloc) continue;
        if(getenv("TRACE")) { fprintf(stderr, "[trace] %s : %s\n", path, proto->name); fflush(stderr); }
        // non-NULL dummy environment: keeloq decoders bail on NULL; the host
        // keystore shim ignores the pointer and hands back an empty keystore.
        static char dummy_env;
        void* dec = proto->decoder->alloc((SubGhzEnvironment*)&dummy_env);
        SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
        if(proto->decoder->reset) proto->decoder->reset(dec); // device parity: worker resets before feed
        HitFlag hf = {0};
        const char* fd = getenv("FRAMEDUMP");
        int dumping = fd && strcmp(fd, proto->name) == 0;
        if(dumping) {
            g_fd_proto = proto;
            g_fd_dec = dec;
            subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb_dump, &hf);
        } else {
            subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
        }

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

            char scbuf[32] = "";
            if(getenv("SYNTHCHECK") && proto->encoder && proto->encoder->alloc &&
               proto->encoder->deserialize && proto->encoder->yield) {
                snprintf(scbuf, sizeof(scbuf), "\t%s", synthcheck(proto, dec));
            }
            char mnbuf[96] = "";
            if(getenv("SYNTHMONO") && proto->encoder && proto->encoder->alloc &&
               proto->encoder->deserialize && proto->encoder->yield) {
                synthmono(proto, dec, mnbuf, sizeof(mnbuf));
            }
            printf(
                "%s\tDECODED\t%s\thits=%d\t%s\t%s%s%s\n",
                path, proto->name, hf.hit, rt, flat, scbuf, mnbuf);
            furi_string_free(out);
        }
        if(proto->decoder->free) proto->decoder->free(dec);
    }
    free(raw);
    } // for each file arg
    return 0;
}
