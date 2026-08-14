// Host test for lib/subghz/protocols/toyota_keeloq.c (plan B, Task 2).
//
// Part (a) SYNTHETIC (gating PASS): builds RAW duration streams for both
// on-air variants (A = Corolla PWM, B = Tundra/Tacoma NRZ) for a known
// (serial, button, hop) frame per the plan's KAT-derived layout, feeds them
// through the REAL decoder's feed(), and asserts the decoded
// generic.serial/btn match for BOTH variants.
//
// Part (b) REAL CAPTURES (diagnostic evidence, not gating): loads real .sub
// RAW_Data timings for a Corolla (variant-A candidate) and a Tacoma
// (variant-B candidate) capture from the local corpus and feeds them through
// the same decoder, reporting honestly whether they decode. A non-decode
// here is reported as diagnostic tuning data, not silently hidden.
//
// Build (mirrors build_lib.sh's recipe, target swapped to this test):
//   wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_toyota_keeloq.sh'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>

extern const SubGhzProtocol toyota_keeloq_protocol;

// ---- known test frame (shared by both synthetic variants) ----
#define TEST_SERIAL 0x1234567UL // 28-bit
#define TEST_BUTTON 0x2UL // Unlock
#define TEST_HOP    0xCAFEBABEUL // 32-bit, ENCRYPTED/raw

// Variant A: 4 leading pad bits ahead of the 64-bit payload (68 total).
#define PAD_A_BITS 4U
#define PAD_A_VAL  0xBU // 0b1011 -- arbitrary, exercises the discard path

// Variant B: 3 leading pad bits ahead of the 64-bit payload (67 total).
// First two pad bits chosen (1,0) so the preamble->data transition pair
// (HIGH=long, LOW=short) can never be mistaken for a preamble pair (which
// requires HIGH=short) -- see toyota_keeloq.c's BAwaitLow comment.
#define PAD_B_BITS 3U
#define PAD_B_VAL  0x4U // 0b100

typedef struct {
    int hit;
} HitFlag;

static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

static void emit(int* durs, size_t* n, size_t cap, bool level, uint32_t us) {
    if(*n >= cap) {
        fprintf(stderr, "emit(): RAW buffer overflow\n");
        exit(1);
    }
    durs[(*n)++] = level ? (int)us : -(int)us;
}

// Bit `i` (0 = MSB) of the concatenation [pad(pad_bits) || payload(payload_bits)].
static bool stream_bit(
    uint32_t pad,
    uint8_t pad_bits,
    uint64_t payload,
    uint8_t payload_bits,
    int i) {
    if(i < pad_bits) {
        return (pad >> (pad_bits - 1 - i)) & 1U;
    }
    const int j = i - pad_bits;
    return (payload >> (payload_bits - 1 - j)) & 1ULL;
}

static uint64_t build_payload(void) {
    return ((uint64_t)TEST_HOP << 32) | (TEST_SERIAL << 4) | TEST_BUTTON;
}

// ---- Variant A: Corolla PWM. te_short=400, te_long=800. ----
static size_t build_variant_a(int* durs, size_t cap) {
    size_t n = 0;
    const uint64_t payload = build_payload();

    for(int i = 0; i < 8; i++) { // >=6 SS preamble pairs (8 used, within spec)
        emit(durs, &n, cap, true, 400);
        emit(durs, &n, cap, false, 400);
    }

    for(int i = 0; i < 68; i++) {
        const bool bit = stream_bit(PAD_A_VAL, PAD_A_BITS, payload, 64, i);
        if(bit) {
            // bit 1 = SL pair: short HIGH + long LOW
            emit(durs, &n, cap, true, 400);
            emit(durs, &n, cap, false, 800);
        } else {
            // bit 0 = LS pair: long HIGH + short LOW
            emit(durs, &n, cap, true, 800);
            emit(durs, &n, cap, false, 400);
        }
    }
    return n;
}

// ---- Variant B: Tundra/Tacoma NRZ. te_short=200, te_long=390, midpoint=287. ----
static size_t build_variant_b(int* durs, size_t cap) {
    size_t n = 0;
    const uint64_t payload = build_payload();

    for(int i = 0; i < 8; i++) { // preamble: short-HIGH + long-LOW pairs
        emit(durs, &n, cap, true, 200);
        emit(durs, &n, cap, false, 390);
    }

    // Transition pair carries the first two stream bits (pad bits 0,1).
    const bool b0 = stream_bit(PAD_B_VAL, PAD_B_BITS, payload, 64, 0);
    const bool b1 = stream_bit(PAD_B_VAL, PAD_B_BITS, payload, 64, 1);
    emit(durs, &n, cap, true, b0 ? 390 : 200);
    emit(durs, &n, cap, false, b1 ? 390 : 200);

    bool level = true; // alternating from the LOW leg just emitted
    for(int i = 2; i < 67; i++) {
        const bool bit = stream_bit(PAD_B_VAL, PAD_B_BITS, payload, 64, i);
        emit(durs, &n, cap, level, bit ? 390 : 200);
        level = !level;
    }
    return n;
}

// Parses "Sn:%07lX  Btn:%01X" / "Hop(raw,ENC):%08lX" / "Variant:%s" out of
// get_string() output.
static bool parse_fields(
    const char* str,
    unsigned long* serial,
    unsigned long* btn,
    unsigned long* hop,
    char* variant_out,
    size_t variant_cap) {
    if(sscanf(str, "%*[^\n]\nVariant:%63[^\r\n]", variant_out) != 1) {
        variant_out[0] = 0;
    }
    if(strlen(variant_out) >= variant_cap) variant_out[variant_cap - 1] = 0;

    const char* sn_field = strstr(str, "Sn:");
    if(!sn_field || sscanf(sn_field, "Sn:%lX  Btn:%lX", serial, btn) != 2) {
        return false;
    }
    const char* hop_field = strstr(str, "Hop(raw,ENC):");
    if(hop_field) sscanf(hop_field, "Hop(raw,ENC):%lX", hop);
    return true;
}

// Runs one synthetic variant through the real decoder; returns 0 on PASS.
static int run_synthetic_variant(const char* label, int* durs, size_t n) {
    static char env;
    void* dec = toyota_keeloq_protocol.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);

    for(size_t i = 0; i < n; i++) {
        int v = durs[i];
        toyota_keeloq_protocol.decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }

    if(hf.hit == 0) {
        fprintf(stderr, "FAIL[%s]: decoder did not fire (hits=0)\n", label);
        toyota_keeloq_protocol.decoder->free(dec);
        return 1;
    }

    FuriString* out = furi_string_alloc();
    toyota_keeloq_protocol.decoder->get_string(dec, out);
    const char* str = furi_string_get_cstr(out);
    printf("[%s] get_string() output:\n%s\n", label, str);

    unsigned long got_serial = 0, got_btn = 0, got_hop = 0;
    char variant_str[64];
    bool ok = parse_fields(str, &got_serial, &got_btn, &got_hop, variant_str, sizeof(variant_str));
    if(!ok) {
        fprintf(stderr, "FAIL[%s]: could not parse Sn/Btn from get_string() output\n", label);
        furi_string_free(out);
        toyota_keeloq_protocol.decoder->free(dec);
        return 1;
    }

    printf(
        "[%s] parsed: serial=0x%07lX btn=0x%lX hop=0x%08lX variant=%s\n",
        label,
        got_serial,
        got_btn,
        got_hop,
        variant_str);

    bool pass = true;
    if(got_serial != TEST_SERIAL) {
        fprintf(
            stderr,
            "FAIL[%s]: serial mismatch: got 0x%07lX expected 0x%07lX\n",
            label,
            got_serial,
            (unsigned long)TEST_SERIAL);
        pass = false;
    }
    if(got_btn != TEST_BUTTON) {
        fprintf(
            stderr,
            "FAIL[%s]: button mismatch: got 0x%lX expected 0x%lX\n",
            label,
            got_btn,
            (unsigned long)TEST_BUTTON);
        pass = false;
    }
    if(got_hop != TEST_HOP) {
        fprintf(
            stderr,
            "FAIL[%s]: hop mismatch: got 0x%08lX expected 0x%08lX (raw passthrough must be exact)\n",
            label,
            got_hop,
            (unsigned long)TEST_HOP);
        pass = false;
    }

    furi_string_free(out);
    toyota_keeloq_protocol.decoder->free(dec);

    if(!pass) return 1;

    printf(
        "PASS[%s]: toyota_keeloq decoded serial=0x%07lX btn=0x%lX(Unlock) hop=0x%08lX "
        "(hits=%d)\n\n",
        label,
        got_serial,
        got_btn,
        got_hop,
        hf.hit);
    return 0;
}

// ---- Part (b): real .sub captures ----

static int* load_raw(const char* path, size_t* out_n) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    size_t cap = 4096, n = 0;
    int* buf = malloc(cap * sizeof(int));
    char line[8192];
    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "RAW_Data:", 9) != 0) continue;
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
    *out_n = n;
    return buf;
}

// Rough diagnostic: how many pulses land inside each variant's short/long
// windows, to help tune constants if a real capture doesn't decode.
static void print_timing_histogram(const int* durs, size_t n) {
    size_t a_short = 0, a_long = 0, b_short = 0, b_long = 0, other = 0;
    for(size_t i = 0; i < n; i++) {
        uint32_t d = (uint32_t)(durs[i] < 0 ? -durs[i] : durs[i]);
        bool matched = false;
        if(d > 400 - 175 && d < 400 + 175) {
            a_short++;
            matched = true;
        }
        if(d > 800 - 175 && d < 800 + 175) {
            a_long++;
            matched = true;
        }
        if(d > 200 - 80 && d < 200 + 80) {
            b_short++;
            matched = true;
        }
        if(d > 390 - 80 && d < 390 + 80) {
            b_long++;
            matched = true;
        }
        if(!matched) other++;
    }
    printf(
        "  timing histogram (n=%zu): A-short(400+-175)=%zu A-long(800+-175)=%zu "
        "B-short(200+-80)=%zu B-long(390+-80)=%zu other=%zu\n",
        n,
        a_short,
        a_long,
        b_short,
        b_long,
        other);
}

static void run_real_capture(const char* label, const char* path) {
    size_t n = 0;
    int* raw = load_raw(path, &n);
    printf("---- real capture [%s]: %s ----\n", label, path);
    if(!raw || n == 0) {
        printf("  NO_RAW (file missing or no RAW_Data lines found)\n\n");
        if(raw) free(raw);
        return;
    }
    printf("  loaded %zu raw duration samples\n", n);
    print_timing_histogram(raw, n);

    static char env;
    void* dec = toyota_keeloq_protocol.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);

    for(size_t i = 0; i < n; i++) {
        int v = raw[i];
        toyota_keeloq_protocol.decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }

    if(hf.hit) {
        FuriString* out = furi_string_alloc();
        toyota_keeloq_protocol.decoder->get_string(dec, out);
        printf("  DECODED (hits=%d):\n%s\n", hf.hit, furi_string_get_cstr(out));
        furi_string_free(out);
    } else {
        printf(
            "  NO DECODE (hits=0) -- DIAGNOSTIC: real-world capture did not match the "
            "spec constants as implemented. See histogram above for observed vs "
            "expected timing distribution.\n\n");
    }

    toyota_keeloq_protocol.decoder->free(dec);
    free(raw);
}

int main(void) {
    static int durs[8192];
    int failures = 0;

    // ---- Part (a): synthetic, gating ----
    size_t na = build_variant_a(durs, 8192);
    failures += run_synthetic_variant("VariantA-Corolla-PWM", durs, na);

    size_t nb = build_variant_b(durs, 8192);
    failures += run_synthetic_variant("VariantB-Tundra-NRZ", durs, nb);

    if(failures) {
        fprintf(stderr, "SYNTHETIC GATING TESTS FAILED (%d failure(s))\n", failures);
        return 1;
    }
    printf("ALL SYNTHETIC TESTS PASS.\n\n");

    // ---- Part (b): real captures, diagnostic (does not affect exit code) ----
    run_real_capture(
        "Corolla (variant-A candidate)",
        "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/Asian/Lexus_Toyota/"
        "Toyota_Corolla_l2_u2_t2.sub");
    run_real_capture(
        "Tacoma (variant-B candidate)",
        "/mnt/c/Users/stone/Downloads/auto-subghz-kakuzu/Asian/Lexus_Toyota/"
        "Toyota_Tacoma_l1_u1.sub");

    return 0;
}
