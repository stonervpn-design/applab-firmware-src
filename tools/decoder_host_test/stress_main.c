/*
 * stress_main.c — anti-phantom host validation of the MAIN/standard-OOK registry.
 *
 * Feeds a Flipper RAW .sub pulse train into every non-TPMS, non-RAW decoder in
 * subghz_protocol_registry_items[] (protocol_items.c): the gate/garage/keeloq/
 * secplus/etc. standard-OOK set. Same feed model as pp_decode_full
 * (decoder->feed(dec, level, duration) per edge) + get_string print on a hit.
 *
 * Purpose: prove noise / degenerate PWM/PPM captures do NOT produce phantom
 * decodes. Any HIT line on a stress/noise file is a phantom to be gated.
 *
 * Build:  bash tools/decoder_host_test/build_stress_main.sh
 * Run:    ./tools/decoder_host_test/stress_main <capture.sub> [more.sub ...]
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

/* ---- MAIN registry, non-TPMS/non-RAW decoders (exact exported symbols) ---- */
extern const SubGhzProtocol subghz_protocol_gate_tx;
extern const SubGhzProtocol subghz_protocol_keeloq;
extern const SubGhzProtocol subghz_protocol_nice_flo;
extern const SubGhzProtocol subghz_protocol_came;
extern const SubGhzProtocol subghz_protocol_faac_slh;
extern const SubGhzProtocol subghz_protocol_nice_flor_s;
extern const SubGhzProtocol subghz_protocol_came_twee;
extern const SubGhzProtocol subghz_protocol_came_atomo;
extern const SubGhzProtocol subghz_protocol_nero_sketch;
extern const SubGhzProtocol subghz_protocol_ido;
extern const SubGhzProtocol subghz_protocol_hormann;
extern const SubGhzProtocol subghz_protocol_nero_radio;
extern const SubGhzProtocol subghz_protocol_somfy_telis;
extern const SubGhzProtocol subghz_protocol_somfy_keytis;
extern const SubGhzProtocol subghz_protocol_princeton;
extern const SubGhzProtocol subghz_protocol_retekess;
extern const SubGhzProtocol subghz_protocol_linear;
extern const SubGhzProtocol subghz_protocol_secplus_v2;
extern const SubGhzProtocol subghz_protocol_secplus_v1;
extern const SubGhzProtocol subghz_protocol_megacode;
extern const SubGhzProtocol subghz_protocol_holtek;
extern const SubGhzProtocol subghz_protocol_chamb_code;
extern const SubGhzProtocol subghz_protocol_power_smart;
extern const SubGhzProtocol subghz_protocol_marantec;
extern const SubGhzProtocol subghz_protocol_bett;
extern const SubGhzProtocol subghz_protocol_doitrand;
extern const SubGhzProtocol subghz_protocol_phoenix_v2;
extern const SubGhzProtocol subghz_protocol_honeywell_wdb;
extern const SubGhzProtocol subghz_protocol_magellan;
extern const SubGhzProtocol subghz_protocol_intertechno_v3;
extern const SubGhzProtocol subghz_protocol_clemsa;
extern const SubGhzProtocol subghz_protocol_ansonic;
extern const SubGhzProtocol subghz_protocol_smc5326;
extern const SubGhzProtocol subghz_protocol_holtek_th12x;
extern const SubGhzProtocol subghz_protocol_linear_delta3;
extern const SubGhzProtocol subghz_protocol_dooya;
extern const SubGhzProtocol subghz_protocol_alutech_at_4n;
extern const SubGhzProtocol subghz_protocol_kinggates_stylo_4k;
extern const SubGhzProtocol subghz_protocol_mastercode;
extern const SubGhzProtocol subghz_protocol_honeywell;
extern const SubGhzProtocol subghz_protocol_legrand;
extern const SubGhzProtocol subghz_protocol_dickert_mahs;
extern const SubGhzProtocol subghz_protocol_gangqi;
extern const SubGhzProtocol subghz_protocol_marantec24;
extern const SubGhzProtocol subghz_protocol_hollarm;
extern const SubGhzProtocol subghz_protocol_hay21;
extern const SubGhzProtocol subghz_protocol_revers_rb2;
extern const SubGhzProtocol subghz_protocol_feron;
extern const SubGhzProtocol subghz_protocol_roger;
extern const SubGhzProtocol subghz_protocol_elplast;
extern const SubGhzProtocol subghz_protocol_treadmill37;
extern const SubGhzProtocol subghz_protocol_beninca_arc;
extern const SubGhzProtocol subghz_protocol_jarolift;
extern const SubGhzProtocol subghz_protocol_ditec_gol4;
extern const SubGhzProtocol subghz_protocol_keyfinder;
extern const SubGhzProtocol subghz_protocol_telcoma_edge;
extern const SubGhzProtocol subghz_protocol_allstar_firefly;
extern const SubGhzProtocol subghz_protocol_nord_ice;
extern const SubGhzProtocol subghz_protocol_x10;
extern const SubGhzProtocol subghz_protocol_hormann_bisecur;
extern const SubGhzProtocol subghz_protocol_cardin_s508;
extern const SubGhzProtocol subghz_protocol_pocsag;
extern const SubGhzProtocol subghz_protocol_genie;
extern const SubGhzProtocol tesla_chargeport_protocol;

static const SubGhzProtocol* const REGISTRY[] = {
    &subghz_protocol_gate_tx,        &subghz_protocol_keeloq,
    &subghz_protocol_nice_flo,       &subghz_protocol_came,
    &subghz_protocol_faac_slh,       &subghz_protocol_nice_flor_s,
    &subghz_protocol_came_twee,      &subghz_protocol_came_atomo,
    &subghz_protocol_nero_sketch,    &subghz_protocol_ido,
    &subghz_protocol_hormann,        &subghz_protocol_nero_radio,
    &subghz_protocol_somfy_telis,    &subghz_protocol_somfy_keytis,
    &subghz_protocol_princeton,      &subghz_protocol_retekess,
    &subghz_protocol_linear,         &subghz_protocol_secplus_v2,
    &subghz_protocol_secplus_v1,     &subghz_protocol_megacode,
    &subghz_protocol_holtek,         &subghz_protocol_chamb_code,
    &subghz_protocol_power_smart,    &subghz_protocol_marantec,
    &subghz_protocol_bett,           &subghz_protocol_doitrand,
    &subghz_protocol_phoenix_v2,     &subghz_protocol_honeywell_wdb,
    &subghz_protocol_magellan,       &subghz_protocol_intertechno_v3,
    &subghz_protocol_clemsa,         &subghz_protocol_ansonic,
    &subghz_protocol_smc5326,        &subghz_protocol_holtek_th12x,
    &subghz_protocol_linear_delta3,  &subghz_protocol_dooya,
    &subghz_protocol_alutech_at_4n,  &subghz_protocol_kinggates_stylo_4k,
    &subghz_protocol_mastercode,     &subghz_protocol_honeywell,
    &subghz_protocol_legrand,        &subghz_protocol_dickert_mahs,
    &subghz_protocol_gangqi,         &subghz_protocol_marantec24,
    &subghz_protocol_hollarm,        &subghz_protocol_hay21,
    &subghz_protocol_revers_rb2,     &subghz_protocol_feron,
    &subghz_protocol_roger,          &subghz_protocol_elplast,
    &subghz_protocol_treadmill37,    &subghz_protocol_beninca_arc,
    &subghz_protocol_jarolift,       &subghz_protocol_ditec_gol4,
    &subghz_protocol_keyfinder,      &subghz_protocol_telcoma_edge,
    &subghz_protocol_allstar_firefly,&subghz_protocol_nord_ice,
    &subghz_protocol_x10,            &subghz_protocol_hormann_bisecur,
    &subghz_protocol_cardin_s508,    &subghz_protocol_pocsag,
    &subghz_protocol_genie,          &tesla_chargeport_protocol,
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
    printf("== MAIN-OOK stress: %d phantom decode(s) ==\n", total_phantoms);
    return total_phantoms ? 1 : 0;
}
