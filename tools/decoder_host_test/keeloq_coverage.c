// KeeLoq master-key console -- Task 2: host coverage matrix.
//
// Proves, for every entry in the host keystore (keystore_host.c), that Task
// 1's registry classification (subghz_keeloq_registry_type_info) matches
// what the REAL lib/subghz/protocols/keeloq.c encoder+decoder actually do:
// every `encodable && KlFamGeneric` entry must genuinely round-trip through
// the real crypto (not just be labelled encodable on paper).
//
// The shipped host keystore (keystore_host.c / keystore_entries.h) loads
// every entry with an EMPTY name -- the kat-decode blob that seeded it
// dropped manufacturer names, keeping only (type, key) pairs. keeloq.c's
// encoder/decoder both pick the encryption key AND the learning-type
// algorithm by scanning the keystore for an EXACT name match, so a shared
// "" name across all entries is fatally ambiguous for a per-entry test.
// This tool renames every loaded entry, in place, to a unique synthetic
// identifier ("KX###") before testing -- that's the only mutation it makes
// to the shared host keystore.
//
// Per generic-encodable entry, the round trip is:
//   1. subghz_protocol_keeloq_create_data() builds a properly-encrypted
//      "capture" (Bit+Key fields) at Cnt=BASE_CNT using that entry's real
//      key/type -- the exact same primitive the real app's by-manufacturer
//      "Generate" flow uses (subghz_txrx_gen_keeloq_protocol() in
//      subghz_txrx_create_protocol_key.c).
//   2. That capture is replayed through the REAL encoder (deserialize +
//      yield), exactly like pressing "Send" on a loaded key file: keeloq.c
//      re-derives serial/btn/cnt from the Key (brute-force restricted to
//      our named entry via a written "Manufacture" field) and then
//      auto-advances the counter by the rolling-counter multiplier (1 on
//      host -- see host_shim.c) before generating the actual RF durations.
//   3. Those durations are fed into the REAL decoder. keystore->mfname is
//      reset to "" first so the decoder performs a genuine unrestricted
//      brute-force identify across every loaded entry (not just the one we
//      built) -- this is the actual coverage assertion, not a shortcut.
//   4. PASS requires: decoded MF == this entry's synthetic name AND decoded
//      Cnt == (BASE_CNT + 1) & 0xFFFF.
//
// Dedicated families (FaacSLH/KingGates/Jarolift/Beninca) print ROUTE-OK --
// their own encoder round-trip is out of this harness's v1 scope (they have
// their own protocol files with their own tests). decode_only and
// KlFamOrphan entries are reported as-is, no round trip attempted.
//
// Build: keeloq_coverage_build.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/protocols/keeloq.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/subghz_keystore_i.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>
#include <m-array.h>

#include "subghz_keeloq_registry.h" // Task 1

#define FIXED_SERIAL 0x123456u
#define FIXED_BTN 0x1u
#define BASE_CNT 100u
#define ROLLING_MULT 1u // matches host_shim.c furi_hal_subghz_get_rolling_counter_mult()
#define DUR_CAP 8192

static void hit_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    (*(int*)ctx)++;
}

// Run the REAL encoder's deserialize->yield pipeline on a seeded
// FlipperFormat (mirrors "press Send on a loaded key"). Returns edge count.
static size_t run_encoder(FlipperFormat* ff, int* durs, size_t cap, const char** reason) {
    static char env;
    *reason = "";
    void* enc = subghz_protocol_keeloq_encoder.alloc((SubGhzEnvironment*)&env);
    flipper_format_rewind(ff);
    SubGhzProtocolStatus st = subghz_protocol_keeloq_encoder.deserialize(enc, ff);
    if(st != SubGhzProtocolStatusOk) {
        subghz_protocol_keeloq_encoder.free(enc);
        *reason = "deser-fail";
        return 0;
    }
    size_t n = 0;
    for(int g = 0; g < (int)cap; g++) {
        LevelDuration ld = subghz_protocol_keeloq_encoder.yield(enc);
        if(level_duration_is_reset(ld)) break;
        int d = level_duration_get_level(ld) ? (int)level_duration_get_duration(ld) :
                                                -(int)level_duration_get_duration(ld);
        if(n < cap) durs[n++] = d;
    }
    subghz_protocol_keeloq_encoder.free(enc);
    if(n == 0) *reason = "no-upload";
    return n;
}

// Feed duration edges into the REAL decoder; return the flattened
// get_string() output (empty if no frame was recognized).
static bool run_decoder(const int* durs, size_t n, char* flat, size_t flatcap) {
    static char env;
    flat[0] = 0;
    void* dec = subghz_protocol_keeloq_decoder.alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    int hit = 0;
    subghz_protocol_decoder_base_set_decoder_callback(base, hit_cb, &hit);
    for(size_t i = 0; i < n; i++) {
        int v = durs[i];
        subghz_protocol_keeloq_decoder.feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }
    // Real captures end in silence; the encoder's yield does not. Flush the
    // decoder's last frame with trailing low-level gaps so end-of-frame commits.
    for(int f = 0; f < 4 && !hit; f++) subghz_protocol_keeloq_decoder.feed(dec, false, 200000u);
    if(hit) {
        FuriString* o = furi_string_alloc();
        subghz_protocol_keeloq_decoder.get_string(dec, o);
        size_t j = 0;
        const char* c = furi_string_get_cstr(o);
        for(; c && *c && j < flatcap - 1; c++) flat[j++] = (*c == '\n' || *c == '\r') ? '|' : *c;
        flat[j] = 0;
        furi_string_free(o);
    }
    subghz_protocol_keeloq_decoder.free(dec);
    return hit != 0;
}

// Extract the token following a label ("MF:", "Cnt:") into out (trimmed word).
static bool field_after(const char* flat, const char* label, char* out, size_t cap) {
    const char* p = strstr(flat, label);
    if(!p) return false;
    p += strlen(label);
    while(*p == ' ') p++;
    size_t j = 0;
    while(*p && *p != ' ' && *p != '|' && j < cap - 1) out[j++] = *p++;
    out[j] = 0;
    return j > 0;
}

// One full create_data -> encoder(deserialize+yield) -> decoder(feed+get_string)
// round trip for `name` at the given (serial, btn, cnt). On success, *mf_tok /
// *decoded_cnt hold the decoded fields and the return is true iff both the
// manufacture name and the advanced counter (cnt + ROLLING_MULT) match.
static bool try_roundtrip(
    SubGhzKeystore* keystore,
    const char* name,
    uint32_t serial,
    uint8_t btn,
    uint16_t cnt,
    char* mf_tok,
    size_t mf_cap,
    uint32_t* decoded_cnt,
    const char** fail_reason) {
    static char env;
    *fail_reason = "";
    mf_tok[0] = 0;
    *decoded_cnt = 0;

    void* enc0 = subghz_protocol_keeloq_encoder.alloc((SubGhzEnvironment*)&env);
    FuriString* preset_name = furi_string_alloc();
    furi_string_cat_str(preset_name, "AM650");
    SubGhzRadioPreset preset = {
        .name = preset_name, .frequency = 433920000, .data = NULL, .data_size = 0};
    FlipperFormat* ff = flipper_format_string_alloc();
    bool built = subghz_protocol_keeloq_create_data(enc0, ff, serial, btn, cnt, name, &preset);
    subghz_protocol_keeloq_encoder.free(enc0);
    furi_string_free(preset_name);
    if(!built) {
        flipper_format_free(ff);
        *fail_reason = "create_data failed";
        return false;
    }

    // Mirrors subghz_txrx_gen_keeloq_protocol(): create_data does not itself
    // write "Manufacture"; the caller does.
    flipper_format_write_string_cstr(ff, "Manufacture", name);

    int durs[DUR_CAP];
    const char* reason;
    size_t nd = run_encoder(ff, durs, DUR_CAP, &reason);
    flipper_format_free(ff);
    if(nd == 0) {
        *fail_reason = reason;
        return false;
    }

    // Force a genuine full brute-force identify on decode -- do not let the
    // encoder-side match leak the answer to the decoder.
    keystore->mfname = "";
    keystore->kl_type = 0;

    char flat[1024];
    if(!run_decoder(durs, nd, flat, sizeof(flat))) {
        *fail_reason = "decoder silent";
        return false;
    }

    char cnt_tok[16] = "";
    field_after(flat, "MF:", mf_tok, mf_cap);
    field_after(flat, "Cnt:", cnt_tok, sizeof(cnt_tok));
    *decoded_cnt = (uint32_t)strtoul(cnt_tok, NULL, 16);

    uint32_t expected_cnt = ((uint32_t)cnt + ROLLING_MULT) & 0xFFFF;
    if(strcmp(mf_tok, name) != 0) {
        *fail_reason = "manufacture mismatch";
        return false;
    }
    if(*decoded_cnt != expected_cnt) {
        *fail_reason = "counter mismatch";
        return false;
    }
    return true;
}

static const char* family_name(KeeloqFamily f) {
    switch(f) {
    case KlFamGeneric: return "Generic";
    case KlFamFaacSLH: return "FaacSLH";
    case KlFamKingGates: return "KingGates";
    case KlFamJarolift: return "Jarolift";
    case KlFamBeninca: return "Beninca";
    case KlFamKiaCar: return "KiaCar";
    case KlFamOrphan: return "Orphan";
    default: return "?";
    }
}

int main(void) {
    SubGhzKeystore* keystore = subghz_environment_get_keystore(NULL);
    SubGhzKeyArray_t* arr = subghz_keystore_get_data(keystore);
    size_t n_entries = SubGhzKeyArray_size(*arr);

    printf("KeeLoq host coverage matrix -- %zu keystore entries loaded\n", n_entries);
    printf("IDX\tTYPE\tFAMILY\tCLASS\tRESULT\tDETAIL\n");
    printf("---\t----\t------\t-----\t------\t------\n");

    // Rename every entry to a unique synthetic identifier up front (see file
    // header). This is the only mutation this tool makes to the shared host
    // keystore; it does not touch key or type.
    {
        size_t i = 0;
        for
            M_EACH(e, *arr, SubGhzKeyArray_t) {
                char nm[16];
                snprintf(nm, sizeof(nm), "KX%03zu", i);
                furi_string_set_str(e->name, nm);
                i++;
            }
    }

    int total = 0, generic_pass = 0, generic_fail = 0, route_ok = 0, decode_only = 0,
        orphaned = 0;
    int types_seen[32] = {0};

    int idx = 0;
    for
        M_EACH(e, *arr, SubGhzKeyArray_t) {
            total++;
            const char* name = furi_string_get_cstr(e->name);
            KeeloqTypeInfo info = subghz_keeloq_registry_type_info(e->type, name);
            const char* fam_s = family_name(info.family);
            if(e->type < 32) types_seen[e->type]++;

            if(info.encodable && info.family == KlFamGeneric) {
                // ---- primary round trip at (FIXED_SERIAL, FIXED_BTN, BASE_CNT) ----
                char mf_tok[32];
                uint32_t decoded_cnt;
                const char* fail_reason;
                bool ok = try_roundtrip(
                    keystore,
                    name,
                    FIXED_SERIAL,
                    FIXED_BTN,
                    (uint16_t)BASE_CNT,
                    mf_tok,
                    sizeof(mf_tok),
                    &decoded_cnt,
                    &fail_reason);

                // KeeLoq's check_decrypt validates only 4 (btn) + ~8 (serial low
                // byte) bits, so with dozens of independent SIMPLE-learning keys
                // sharing this test's fixed serial/btn, an EARLIER wrong key can
                // occasionally false-positive-match a LATER entry's frame (a
                // ~1/2048 chance per wrong-key trial -- a real property of
                // KeeLoq's short discriminator, not an encoder/decoder/registry
                // bug). Retry once at different (serial, btn, cnt): a genuine
                // crypto/registry bug reproduces every time; a discriminator
                // collision essentially never repeats at different inputs.
                bool collision_retry = false;
                if(!ok && strcmp(fail_reason, "manufacture mismatch") == 0) {
                    collision_retry = true;
                    ok = try_roundtrip(
                        keystore,
                        name,
                        FIXED_SERIAL ^ (0x00A50000u + (uint32_t)idx),
                        (uint8_t)(FIXED_BTN == 1 ? 2 : 1),
                        (uint16_t)(BASE_CNT + 500 + idx),
                        mf_tok,
                        sizeof(mf_tok),
                        &decoded_cnt,
                        &fail_reason);
                }

                if(ok) {
                    generic_pass++;
                    printf(
                        "%d\t%u\t%s\tgeneric-encodable\tPASS\tMF:%s Cnt:%04X%s\n",
                        idx,
                        e->type,
                        fam_s,
                        mf_tok,
                        decoded_cnt,
                        collision_retry ? " (after discriminator-collision retry)" : "");
                } else {
                    generic_fail++;
                    printf(
                        "%d\t%u\t%s\tgeneric-encodable\tFAIL\t%s (MF:%s want:%s Cnt:%04X)%s\n",
                        idx,
                        e->type,
                        fam_s,
                        fail_reason,
                        mf_tok,
                        name,
                        decoded_cnt,
                        collision_retry ? " [retry also failed]" : "");
                }
            } else if(info.encodable) {
                // Dedicated family with its own encoder (FAAC/KingGates/Jarolift/
                // Beninca) -- round trip covered by that protocol's own tests.
                route_ok++;
                printf(
                    "%d\t%u\t%s\tROUTE:%s\tROUTE-OK\tdedicated encoder round-trip out of v1 scope\n",
                    idx,
                    e->type,
                    fam_s,
                    fam_s);
            } else if(info.family == KlFamOrphan) {
                orphaned++;
                printf(
                    "%d\t%u\t%s\torphan\tORPHANED\tno known generator route\n",
                    idx,
                    e->type,
                    fam_s);
            } else {
                decode_only++;
                printf(
                    "%d\t%u\t%s\tdecode-only\tDECODE-ONLY\tcatalog-generate not v1-supported\n",
                    idx,
                    e->type,
                    fam_s);
            }
            idx++;
        }

    printf("\n-- learning types present in this keystore snapshot --\n");
    for(int t = 0; t < 32; t++) {
        if(types_seen[t])
            printf("type %2d: %d entr%s\n", t, types_seen[t], types_seen[t] == 1 ? "y" : "ies");
    }

    printf(
        "\nSUMMARY: total=%d generic_PASS=%d generic_FAIL=%d ROUTE-OK=%d DECODE-ONLY=%d ORPHANED=%d\n",
        total,
        generic_pass,
        generic_fail,
        route_ok,
        decode_only,
        orphaned);

    return generic_fail > 0 ? 1 : 0;
}
