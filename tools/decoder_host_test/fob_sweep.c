// Per-protocol, per-button synthesize->decode round-trip sweep for the
// car-fob multi-button emulate feature (Task 4). For every SUPPORTED protocol
// (Task 1's 21) and every button in subghz_fob_buttons(), this:
//   1. seeds a FlipperFormat with a fixed Serial, a fixed counter N and the
//      requested button, written to the protocol's own button field
//      (subghz_fob_button_field), plus any per-protocol required extras;
//   2. runs the REAL lib/subghz encoder (deserialize -> pump yield to durations);
//   3. decodes the produced duration stream with the REAL decoder;
//   4. asserts the decoded button == the requested button (name or code match).
// It also seeds N+1/N+2 (button fixed) and records whether the decoded counter
// advances (true synth) or stays fixed (replay-rolling-field).
//
// Output: a PASS/FAIL/REPLAY matrix, one line per (protocol,button), plus a
// per-protocol counter classification and a final summary. Build: build_lib.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>

#include "subghz_fob_buttons.h" // provider under test

// ---- the 21 SUPPORTED protocols (extern from lib/subghz/protocols/*.c) ----
extern const SubGhzProtocol vag_protocol;
extern const SubGhzProtocol subghz_protocol_keeloq;
extern const SubGhzProtocol kia_protocol_v0;
extern const SubGhzProtocol kia_protocol_v1;
extern const SubGhzProtocol kia_protocol_v7;
extern const SubGhzProtocol hyundai_kia_rio_protocol;
extern const SubGhzProtocol santa_fe_protocol;
extern const SubGhzProtocol ford_protocol_v0;
extern const SubGhzProtocol ford_protocol_v1;
extern const SubGhzProtocol ford_protocol_v3;
extern const SubGhzProtocol honda_static_protocol;
extern const SubGhzProtocol subghz_protocol_honda_v2_v3;
extern const SubGhzProtocol subghz_protocol_honda_v5;
extern const SubGhzProtocol subghz_protocol_honda_v7;
extern const SubGhzProtocol subghz_protocol_honda_v8;
extern const SubGhzProtocol subghz_protocol_honda_v9;
extern const SubGhzProtocol mazda_v0_protocol;
extern const SubGhzProtocol subaru_protocol;
extern const SubGhzProtocol fiat_v1_protocol;
extern const SubGhzProtocol chrysler_protocol_v0;
extern const SubGhzProtocol land_rover_v0_protocol;

typedef struct {
    const char* name; // provider protocol-name string
    const SubGhzProtocol* proto;
    uint16_t bits; // Bit field to seed
    const char* cnt_field; // counter field the encoder reads
} ProtoCfg;

static const ProtoCfg CFG[] = {
    {"VAG", &vag_protocol, 80, "Cnt"},
    {"KeeLoq", &subghz_protocol_keeloq, 64, "Cnt"},
    {"Kia V0", &kia_protocol_v0, 61, "Cnt"},
    {"Kia V1", &kia_protocol_v1, 57, "Cnt"},
    {"Kia V7", &kia_protocol_v7, 64, "Cnt"},
    {"Hyundai/Kia RIO", &hyundai_kia_rio_protocol, 64, "Cnt"},
    {"SantaFe 13-16", &santa_fe_protocol, 80, "Cnt"},
    {"Ford V0", &ford_protocol_v0, 64, "Cnt"},
    {"Ford V1", &ford_protocol_v1, 136, "Cnt"},
    {"Ford V3", &ford_protocol_v3, 104, "Cnt"},
    {"Honda Static", &honda_static_protocol, 64, "Cnt"},
    {"HondaV2&V3", &subghz_protocol_honda_v2_v3, 148, "Rolling"},
    {"HondaV5", &subghz_protocol_honda_v5, 135, "Counter"},
    {"HondaV7", &subghz_protocol_honda_v7, 64, "Counter"},
    {"HondaV8", &subghz_protocol_honda_v8, 64, "RollingCode"},
    {"HondaV9", &subghz_protocol_honda_v9, 64, "Counter"},
    {"Mazda V0", &mazda_v0_protocol, 64, "Cnt"},
    {"Subaru", &subaru_protocol, 64, "Cnt"},
    {"Fiat V1", &fiat_v1_protocol, 102, "Cnt"},
    {"Chrysler V0", &chrysler_protocol_v0, 80, "Cnt"},
    {"Land Rover V0", &land_rover_v0_protocol, 81, "Cnt"},
};
#define CFG_N (sizeof(CFG) / sizeof(CFG[0]))

#define FIXED_SERIAL 0x123456u

static void wr_u32(FlipperFormat* ff, const char* k, uint32_t v) {
    flipper_format_write_uint32(ff, k, &v, 1);
}
static void wr_hex(FlipperFormat* ff, const char* k, const uint8_t* d, size_t n) {
    flipper_format_write_hex(ff, k, d, n);
}

// Emit the button/counter/extra fields IN THE ORDER the encoder reads them.
// The honda_* encoders read fields sequentially (no rewind between reads) after
// the generic Bit/Key block, so field order in the file is load-bearing for
// them; the pp_encoder_read_fields protocols rewind per field (order-agnostic).
static void emit_fields(FlipperFormat* ff, const ProtoCfg* cfg, uint8_t btn, uint32_t cnt) {
    static const uint8_t k8[8] = {0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A};
    static const uint8_t k4[4] = {0x11, 0x22, 0x33, 0x44};
    static const uint8_t k2[2] = {0x00, 0x00};
    static const uint8_t hitag6[6] = {0x4D, 0x49, 0x4B, 0x52, 0x4F, 0x4E};
    const char* n = cfg->name;

    if(!strcmp(n, "HondaV7") || !strcmp(n, "HondaV9")) {
        // reads: Serial (emitted by caller), Counter, Button
        wr_u32(ff, "Counter", cnt);
        wr_u32(ff, "Button", btn);
    } else if(!strcmp(n, "HondaV8")) {
        // reads: Serial, RollingCode, Button, KeyIdx (KeyIdx must be a valid mfr idx)
        wr_u32(ff, "RollingCode", cnt);
        wr_u32(ff, "Button", btn);
        wr_u32(ff, "KeyIdx", 0);
    } else if(!strcmp(n, "HondaV5")) {
        // reads: Header, Retransmit, Rolling, Tail, Button, Counter
        wr_u32(ff, "Header", 0);
        wr_u32(ff, "Retransmit", 0);
        wr_u32(ff, "Rolling", 0);
        wr_u32(ff, "Tail", 0);
        wr_u32(ff, "Button", btn);
        wr_u32(ff, "Counter", cnt);
    } else if(!strcmp(n, "HondaV2&V3")) {
        // reads: Variant, Rolling, Tail, BtnHi, BtnLo
        wr_u32(ff, "Variant", 2); // HondaVariantV2
        wr_u32(ff, "Rolling", cnt);
        wr_u32(ff, "Tail", 0);
        wr_u32(ff, "BtnHi", 0);
        wr_u32(ff, "BtnLo", btn);
    } else {
        // pp_encoder_read_fields family: Btn/Cnt rewind-read, order-agnostic.
        wr_u32(ff, subghz_fob_button_field(n), btn);
        wr_u32(ff, cfg->cnt_field, cnt);
        if(strcmp(cfg->cnt_field, "Cnt") != 0) wr_u32(ff, "Cnt", cnt);
        // per-protocol crypto/extra material
        if(!strcmp(n, "VAG")) {
            wr_hex(ff, "Key2", k8, 8);
            wr_u32(ff, "KeyIdx", 0);
        } else if(!strcmp(n, "KeeLoq")) {
            wr_hex(ff, "Seed", k4, 4);
            flipper_format_write_string_cstr(ff, "Manufacture", "DoorHan");
        } else if(!strcmp(n, "Ford V1")) {
            wr_hex(ff, "Key_2", k8, 8);
            wr_hex(ff, "Key_3", k4, 4);
        } else if(!strcmp(n, "Chrysler V0")) {
            wr_hex(ff, "Key_2", k2, 2);
        } else if(!strcmp(n, "Fiat V1")) {
            wr_hex(ff, "Hitag2 Key", hitag6, 6);
        }
    }
}

// Build a fresh seeded FlipperFormat for (cfg, button, cnt).
static FlipperFormat* seed(const ProtoCfg* cfg, uint8_t btn, uint32_t cnt) {
    static const uint8_t key[8] = {0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A};
    FlipperFormat* ff = flipper_format_string_alloc();
    flipper_format_write_header_cstr(ff, "Flipper SubGhz Key File", 1);
    flipper_format_write_string_cstr(ff, "Protocol", cfg->name);
    wr_u32(ff, "Bit", cfg->bits);
    wr_hex(ff, "Key", key, 8);
    wr_u32(ff, "Serial", FIXED_SERIAL); // honda_v7/8/9 read this first, sequentially
    emit_fields(ff, cfg, btn, cnt);
    return ff;
}

typedef struct {
    int hit;
} HitFlag;
static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}
static void flatten(const char* c, char* out, size_t cap) {
    size_t j = 0;
    for(; c && *c && j < cap - 1; c++) out[j++] = (*c == '\n' || *c == '\r') ? '|' : *c;
    out[j] = 0;
}
static void lower(char* s) {
    for(; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// Encode a seed to a duration buffer. Returns count (0 on failure). reason set.
static size_t encode(const SubGhzProtocol* proto, FlipperFormat* ff, int* durs, size_t cap, const char** reason) {
    static char env;
    *reason = "";
    if(!proto->encoder || !proto->encoder->alloc || !proto->encoder->deserialize ||
       !proto->encoder->yield) {
        *reason = "no-encoder";
        return 0;
    }
    void* enc = proto->encoder->alloc((SubGhzEnvironment*)&env);
    flipper_format_rewind(ff);
    SubGhzProtocolStatus st = proto->encoder->deserialize(enc, ff);
    if(st != SubGhzProtocolStatusOk) {
        if(proto->encoder->free) proto->encoder->free(enc);
        *reason = "deser-fail";
        return 0;
    }
    size_t n = 0;
    for(int g = 0; g < (int)cap; g++) {
        LevelDuration ld = proto->encoder->yield(enc);
        if(level_duration_is_reset(ld)) break;
        int d = level_duration_get_level(ld) ? (int)level_duration_get_duration(ld) :
                                               -(int)level_duration_get_duration(ld);
        if(n < cap) durs[n++] = d;
    }
    if(proto->encoder->free) proto->encoder->free(enc);
    if(n == 0) *reason = "no-upload";
    return n;
}

// Decode a duration buffer; fill flat[] with flattened get_string. Returns hits.
static int decode(const SubGhzProtocol* proto, const int* durs, size_t n, char* flat, size_t flatcap) {
    static char env;
    flat[0] = 0;
    if(!proto->decoder || !proto->decoder->alloc) return 0;
    void* dec = proto->decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
    for(size_t s = 0; s < n; s++) {
        int v = durs[s];
        proto->decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    // Real captures end in silence; the encoder's yield does not. Flush the
    // decoder's last frame with trailing low-level gaps so end-of-frame commits.
    for(int f = 0; f < 4 && !hf.hit; f++) proto->decoder->feed(dec, false, 200000u);
    if(hf.hit && proto->decoder->get_string) {
        FuriString* o = furi_string_alloc();
        proto->decoder->get_string(dec, o);
        flatten(furi_string_get_cstr(o), flat, flatcap);
        furi_string_free(o);
    }
    if(proto->decoder->free) proto->decoder->free(dec);
    return hf.hit;
}

// Extract the token following a label ("Btn:", "Cnt:") into out (trimmed word).
static bool field_after(const char* flat, const char* label, char* out, size_t cap) {
    const char* p = strstr(flat, label);
    if(!p) return false;
    p += strlen(label);
    while(*p == ' ') p++;
    size_t j = 0;
    while(*p && *p != ' ' && *p != '|' && *p != '[' && *p != '(' && j < cap - 1) out[j++] = *p++;
    out[j] = 0;
    return j > 0;
}

// Does the decoded string reflect the requested button (by hex code or by name)?
static bool button_ok(const char* flat, uint8_t code, const char* bname) {
    char tok[64];
    if(field_after(flat, "Btn:", tok, sizeof(tok))) {
        // numeric match (hex, then decimal)
        char* end;
        unsigned long hx = strtoul(tok, &end, 16);
        if(end != tok && hx == code) return true;
        unsigned long dc = strtoul(tok, &end, 10);
        if(end != tok && dc == code) return true;
    }
    // name match (case-insensitive substring)
    char lf[1024];
    char ln[128];
    strncpy(lf, flat, sizeof(lf) - 1);
    lf[sizeof(lf) - 1] = 0;
    lower(lf);
    strncpy(ln, bname, sizeof(ln) - 1);
    ln[sizeof(ln) - 1] = 0;
    lower(ln);
    return strstr(lf, ln) != NULL;
}

// ---- capture-seeded verification (mirrors the firmware: decode a real .sub,
// change the button, re-encode, decode, assert) ----
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
    *out_n = n;
    return buf;
}

#define COLL "/mnt/c/Users/stone/Downloads/auto_subghz_test/Automotive-Sub-Ghz-Collection-main"
typedef struct {
    const char* name;
    const char* paths[7]; // NULL-terminated candidate list (lib decoder must fire)
} CapEntry;
static const CapEntry CAPS[] = {
    {"VAG",
     {COLL "/European/VW_Audi_Skoda_Seat/Skoda_Fabia_2007 RAW.sub",
      COLL "/European/VW_Audi_Skoda_Seat/VW_Golf4.sub",
      COLL "/European/VW_Audi_Skoda_Seat/VW_Passat_2015.sub",
      COLL "/European/VW_Audi_Skoda_Seat/VW_Passat_B7.sub",
      COLL "/European/VW_Audi_Skoda_Seat/VW_Skoda_Seat_2008.sub",
      COLL "/European/VW_Audi_Skoda_Seat/VW_VAG_2.sub", NULL}},
    {"Kia V0", {COLL "/Asian/Hyundai_Kia_Genesis/Kia_0_orig.sub", NULL}},
    {"Kia V1",
     {COLL "/Asian/Hyundai_Kia_Genesis/Kia_V1_5cl_5op.sub",
      COLL "/Asian/Hyundai_Kia_Genesis/Kia_V1_N1_RAW.sub",
      COLL "/Asian/Hyundai_Kia_Genesis/Hyundai_V1_N1_RAW.sub", NULL}},
    {"Ford V0", {COLL "/American/Ford/Ford_Escape_l2_u2_rs2_t2_p2.sub", NULL}},
    {"Ford V3",
     {COLL "/Asian/Suzuki_Isuzu/LDV_T80_lock.sub",
      COLL "/Asian/Suzuki_Isuzu/LDV_T80_unlock.sub", NULL}},
    {"Subaru", {COLL "/Asian/Nissan/Subaru_Impreza RAW .sub", NULL}},
    {"Honda Static",
     {COLL "/Asian/Honda_Acura/Honda_Lock.sub",
      COLL "/Asian/Honda_Acura/Honda_Unlock.sub", NULL}},
};
#define CAPS_N (sizeof(CAPS) / sizeof(CAPS[0]))

// Decode a real capture into a baseline serialized FlipperFormat (valid Key +
// crypto material). Returns NULL if the protocol did not fire on the capture.
static FlipperFormat* baseline_from_capture(const SubGhzProtocol* proto, const char* path) {
    static char env;
    size_t rn = 0;
    int* raw = load_raw(path, &rn);
    if(!raw || rn == 0) {
        free(raw);
        return NULL;
    }
    void* dec = proto->decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);
    for(size_t s = 0; s < rn; s++) {
        int v = raw[s];
        proto->decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    free(raw);
    FlipperFormat* ff = NULL;
    if(hf.hit && proto->decoder->serialize) {
        FuriString* pn = furi_string_alloc();
        furi_string_cat_str(pn, "AM650");
        SubGhzRadioPreset preset = {
            .name = pn, .frequency = 433920000, .data = NULL, .data_size = 0};
        ff = flipper_format_string_alloc();
        if(proto->decoder->serialize(dec, ff, &preset) != SubGhzProtocolStatusOk) {
            flipper_format_free(ff);
            ff = NULL;
        }
        furi_string_free(pn);
    }
    if(proto->decoder->free) proto->decoder->free(dec);
    return ff;
}

int main(void) {
    int durs[32768];
    int total = 0, pass = 0, fail = 0, seedfail = 0;
    printf("PROTO\tBUTTON(code)\tRESULT\tDECODED_BTN_FIELD / reason\n");
    printf("-----\t------------\t------\t-------------------------\n");

    for(size_t i = 0; i < CFG_N; i++) {
        const ProtoCfg* cfg = &CFG[i];
        size_t nbtn = 0;
        const SubGhzFobButton* btns = subghz_fob_buttons(cfg->name, &nbtn);
        if(!btns || nbtn == 0) {
            printf("%s\t(none)\tNOMAP\tprovider returned no table\n", cfg->name);
            continue;
        }
        for(size_t b = 0; b < nbtn; b++) {
            total++;
            const char* reason = "";
            FlipperFormat* ff = seed(cfg, btns[b].code, 100);
            size_t n = encode(cfg->proto, ff, durs, 32768, &reason);
            flipper_format_free(ff);
            if(n == 0) {
                seedfail++;
                printf(
                    "%s\t%s(0x%X)\tSEED-FAIL\t%s\n", cfg->name, btns[b].name, btns[b].code, reason);
                continue;
            }
            char flat[1024];
            int hit = decode(cfg->proto, durs, n, flat, sizeof(flat));
            if(!hit) {
                fail++;
                printf(
                    "%s\t%s(0x%X)\tNODECODE\tencoded %zu edges, decoder silent\n",
                    cfg->name,
                    btns[b].name,
                    btns[b].code,
                    n);
                continue;
            }
            char btntok[64];
            field_after(flat, "Btn:", btntok, sizeof(btntok));
            if(button_ok(flat, btns[b].code, btns[b].name)) {
                pass++;
                printf(
                    "%s\t%s(0x%X)\tPASS\tBtn:%s\n", cfg->name, btns[b].name, btns[b].code, btntok);
            } else {
                fail++;
                printf(
                    "%s\t%s(0x%X)\tFAIL\tdecoded Btn:%s (wanted 0x%X/%s)\n",
                    cfg->name,
                    btns[b].name,
                    btns[b].code,
                    btntok,
                    btns[b].code,
                    btns[b].name);
            }
        }

        // ---- counter behavior: fix button[0], seed N, N+1, N+2, read Cnt ----
        char c0[64] = "?", c1[64] = "?", c2[64] = "?";
        const char* r;
        char flat[1024];
        uint8_t fb = btns[0].code;
        FlipperFormat* f0 = seed(cfg, fb, 1000);
        size_t n0 = encode(cfg->proto, f0, durs, 32768, &r);
        flipper_format_free(f0);
        if(n0 && decode(cfg->proto, durs, n0, flat, sizeof(flat))) field_after(flat, "Cnt:", c0, sizeof(c0));
        FlipperFormat* f1 = seed(cfg, fb, 1001);
        size_t n1 = encode(cfg->proto, f1, durs, 32768, &r);
        flipper_format_free(f1);
        if(n1 && decode(cfg->proto, durs, n1, flat, sizeof(flat))) field_after(flat, "Cnt:", c1, sizeof(c1));
        FlipperFormat* f2 = seed(cfg, fb, 1002);
        size_t n2 = encode(cfg->proto, f2, durs, 32768, &r);
        flipper_format_free(f2);
        if(n2 && decode(cfg->proto, durs, n2, flat, sizeof(flat))) field_after(flat, "Cnt:", c2, sizeof(c2));

        const char* cls;
        if(!strcmp(c0, "?") || !strcmp(c1, "?") || !strcmp(c2, "?"))
            cls = "CNT-UNREAD";
        else if(strcmp(c0, c1) != 0 && strcmp(c1, c2) != 0 && strcmp(c0, c2) != 0)
            cls = "CNT-ADVANCES(synth)";
        else if(!strcmp(c0, c1) && !strcmp(c1, c2))
            cls = "CNT-FIXED(replay-field)";
        else
            cls = "CNT-PARTIAL";
        printf("# %s COUNTER: %s  [N=%s N+1=%s N+2=%s]\n", cfg->name, cls, c0, c1, c2);
    }

    // ================= capture-seeded pass (firmware-accurate) =================
    printf("\n== CAPTURE-SEEDED (decode real .sub -> change button -> re-encode -> decode) ==\n");
    int cpass = 0, cfail = 0;
    for(size_t i = 0; i < CAPS_N; i++) {
        const ProtoCfg* cfg = NULL;
        for(size_t j = 0; j < CFG_N; j++)
            if(!strcmp(CFG[j].name, CAPS[i].name)) cfg = &CFG[j];
        if(!cfg) continue;
        size_t nbtn = 0;
        const SubGhzFobButton* btns = subghz_fob_buttons(cfg->name, &nbtn);
        FlipperFormat* base = NULL;
        for(size_t pi = 0; CAPS[i].paths[pi] && !base; pi++)
            base = baseline_from_capture(cfg->proto, CAPS[i].paths[pi]);
        if(!base) {
            printf(
                "CAP %s\t--\tNO-BASELINE\tno candidate capture decoded as this protocol\n",
                cfg->name);
            continue;
        }
        const char* bfield = subghz_fob_button_field(cfg->name);
        char cprev[64] = "";
        int cnt_moves = 0, cnt_runs = 0;
        for(size_t b = 0; b < nbtn; b++) {
            uint32_t code = btns[b].code;
            flipper_format_rewind(base);
            flipper_format_insert_or_update_uint32(base, bfield, &code, 1);
            const char* r;
            size_t n = encode(cfg->proto, base, durs, 32768, &r);
            char flat[1024];
            if(!n || !decode(cfg->proto, durs, n, flat, sizeof(flat))) {
                cfail++;
                printf(
                    "CAP %s\t%s(0x%X)\t%s\t%s\n",
                    cfg->name,
                    btns[b].name,
                    btns[b].code,
                    n ? "NODECODE" : "SEED-FAIL",
                    n ? "re-encoded but silent" : r);
                continue;
            }
            char tok[64];
            field_after(flat, "Btn:", tok, sizeof(tok));
            if(button_ok(flat, btns[b].code, btns[b].name)) {
                cpass++;
                printf("CAP %s\t%s(0x%X)\tPASS\tBtn:%s\n", cfg->name, btns[b].name, btns[b].code, tok);
            } else {
                cfail++;
                printf(
                    "CAP %s\t%s(0x%X)\tREPLAY\tdecoded Btn:%s (button not honoured)\n",
                    cfg->name,
                    btns[b].name,
                    btns[b].code,
                    tok);
            }
        }
        // counter advance on the captured baseline: bump Cnt across 3 runs
        uint32_t fb = btns[0].code;
        for(uint32_t k = 0; k < 3; k++) {
            flipper_format_rewind(base);
            flipper_format_insert_or_update_uint32(base, bfield, &fb, 1);
            uint32_t cv = 2000 + k;
            flipper_format_rewind(base);
            flipper_format_insert_or_update_uint32(base, "Cnt", &cv, 1);
            const char* r;
            size_t n = encode(cfg->proto, base, durs, 32768, &r);
            char flat[1024], ctok[64] = "?";
            if(n && decode(cfg->proto, durs, n, flat, sizeof(flat))) {
                field_after(flat, "Cnt:", ctok, sizeof(ctok));
                cnt_runs++;
                if(k > 0 && strcmp(ctok, cprev) != 0) cnt_moves++;
            }
            strncpy(cprev, ctok, sizeof(cprev) - 1);
        }
        printf(
            "# CAP %s COUNTER: %s (runs=%d moves=%d)\n",
            cfg->name,
            (cnt_runs == 3 && cnt_moves == 2) ? "ADVANCES(synth)" :
                (cnt_runs == 3 && cnt_moves == 0) ? "FIXED(replay-field)" : "PARTIAL/UNREAD",
            cnt_runs,
            cnt_moves);
        flipper_format_free(base);
    }

    printf(
        "\nSUMMARY(synthetic): total=%d PASS=%d FAIL=%d SEED-FAIL=%d\n",
        total,
        pass,
        fail,
        seedfail);
    printf("SUMMARY(capture-seeded): PASS=%d NON-PASS=%d\n", cpass, cfail);
    return 0;
}
