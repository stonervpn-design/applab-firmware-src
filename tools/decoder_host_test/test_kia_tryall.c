// Regression test for the Kia V3/V4 "try all manufacturer keys" fix.
//
// The keystore legitimately carries several distinct KIA_KEY1 (type 10) entries
// all tagged "KIA_Hyundai_V3&4" (different model years / regions). The old
// loader kept only the LAST one (kia_mf_key = <each type-10>), so any fob keyed
// to an earlier entry silently failed to decode. The fix keeps ALL type-10 keys
// in kia_mf_keys[] and the decoder tries each.
//
// This test proves both halves:
//   (1) LOAD: after protopirate_keys_load(), the array holds every distinct
//       type-10 key, the canonical A8F5DFFC... is present, and the legacy
//       single slot still equals the LAST one (CB72B957...) -- i.e. the old
//       code path would have used the wrong key.
//   (2) DECODE: a synthetic V4 frame KeeLoq-encrypted with the canonical
//       A8F5DFFC... key (NOT the last-wins key) now decodes end-to-end.
//
// Build+run (from repo root under WSL):
//   wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_kia_tryall.sh'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <core/furi_string.h>

extern const SubGhzProtocol kia_protocol_v3_v4;

// keys API under test. NOTE: this test links the lib/subghz copy of keys.c,
// whose protopirate_keys_load returns void (the proto_pirate copy returns bool);
// declare void to match the definition we actually link, and use
// get_kia_mf_key_count() as the "did it load" signal instead of a return value.
extern void protopirate_keys_load(SubGhzEnvironment* environment);
extern uint64_t get_kia_mf_key(void);
extern uint8_t get_kia_mf_key_count(void);
extern uint64_t get_kia_mf_key_at(uint8_t index);

// KeeLoq + bit helper from the real firmware
extern uint32_t subghz_protocol_keeloq_common_encrypt(const uint32_t data, const uint64_t key);
extern uint8_t pp_reverse_bits8(uint8_t v);

#define KEY_CANON 0xA8F5DFFC8DAA5CDBULL // canonical Kia/Hyundai V3&4 (keystore line 175)
#define KEY_LAST  0xCB72B95709057920ULL // duplicate added later (keystore line 197) -> last-wins

#define SERIAL 0x00C0ED06UL // 28-bit, top byte 0 (so b7 high nibble carries only serial-top-nibble=0)
#define BTN    0x2UL // 4-bit button
#define CNT    0x1234UL // 16-bit counter

typedef struct {
    int hit;
} HitFlag;
static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

static int g_n = 0;
static int g_durs[4096];
static void emit(bool level, uint32_t us) {
    if(g_n >= (int)(sizeof(g_durs) / sizeof(g_durs[0]))) {
        fprintf(stderr, "RAW overflow\n");
        exit(1);
    }
    g_durs[g_n++] = level ? (int)us : -(int)us;
}

int main(void) {
    int fails = 0;

    // ---- (1) LOAD ---------------------------------------------------------
    protopirate_keys_load((SubGhzEnvironment*)0x1);
    uint8_t nk = get_kia_mf_key_count();
    if(nk == 0) {
        fprintf(stderr, "FAIL: protopirate_keys_load loaded no KIA_KEY1 keys\n");
        return 1;
    }
    printf("[load] kia_mf_key_count=%u  legacy get_kia_mf_key()=%016llX\n",
           nk, (unsigned long long)get_kia_mf_key());
    bool canon_present = false, last_present = false;
    for(uint8_t i = 0; i < nk; i++) {
        uint64_t k = get_kia_mf_key_at(i);
        printf("   key[%u]=%016llX\n", i, (unsigned long long)k);
        if(k == KEY_CANON) canon_present = true;
        if(k == KEY_LAST) last_present = true;
    }
    if(nk < 2) { fprintf(stderr, "FAIL: expected >=2 type-10 keys, got %u\n", nk); fails++; }
    if(!canon_present) { fprintf(stderr, "FAIL: canonical A8F5DFFC... not in key set\n"); fails++; }
    if(!last_present) { fprintf(stderr, "FAIL: CB72B957... not in key set\n"); fails++; }
    if(get_kia_mf_key() != KEY_LAST)
        fprintf(stderr,
                "NOTE: legacy slot != CB72B957 (keystore order changed); test still valid if canon present\n");
    else
        printf("[load] confirmed: OLD last-wins path would use CB72B957 (wrong key)\n");

    // ---- (2) DECODE a canonical-key frame ---------------------------------
    // plaintext: top nibble = button, bits[23:16] = serial LSB (KeeLoq disc).
    uint32_t plain = ((uint32_t)BTN << 28) | ((SERIAL & 0xFFU) << 16) | (uint32_t)(CNT & 0xFFFFU);
    uint32_t enc = subghz_protocol_keeloq_common_encrypt(plain, KEY_CANON);

    uint8_t b[9];
    b[0] = pp_reverse_bits8(enc & 0xFF);
    b[1] = pp_reverse_bits8((enc >> 8) & 0xFF);
    b[2] = pp_reverse_bits8((enc >> 16) & 0xFF);
    b[3] = pp_reverse_bits8((enc >> 24) & 0xFF);
    b[4] = pp_reverse_bits8(SERIAL & 0xFF);
    b[5] = pp_reverse_bits8((SERIAL >> 8) & 0xFF);
    b[6] = pp_reverse_bits8((SERIAL >> 16) & 0xFF);
    // b[7] carries serial top-nibble (in high nibble, reversed) + button; brute it
    uint8_t b7 = 0;
    bool found7 = false;
    for(int cand = 0; cand < 256; cand++) {
        uint8_t serial_top = pp_reverse_bits8((uint8_t)cand & 0xF0); // -> compared to (serial>>24)&0xFF
        uint8_t button = (pp_reverse_bits8((uint8_t)cand) & 0xF0) >> 4;
        if(serial_top == ((SERIAL >> 24) & 0xFF) && button == BTN) {
            b7 = (uint8_t)cand;
            found7 = true;
            break;
        }
    }
    if(!found7) { fprintf(stderr, "FAIL: could not construct b7 for serial/btn\n"); return 1; }
    b[7] = b7;
    b[8] = 0x00; // CRC nibble (decoder reads but does not gate accept)

    // waveform: preamble pairs -> V4 sync HIGH(1200) -> 68 PWM bits -> end gap
    g_n = 0;
    for(int i = 0; i < 12; i++) { emit(true, 400); emit(false, 400); }
    emit(true, 1200); // V4 sync (is_v3_sync=false)
    for(int bit = 0; bit < 68; bit++) {
        int byte_idx = bit / 8;
        int bit_idx = 7 - (bit % 8);
        int v = (b[byte_idx] >> bit_idx) & 1;
        emit(true, v ? 800 : 400); // HIGH duration carries the bit
        emit(false, 400); // LOW filler (ignored by decoder)
    }
    emit(false, 2000); // end-of-frame gap -> process_buffer

    HitFlag hf = {0};
    void* dec = kia_protocol_v3_v4.decoder->alloc((SubGhzEnvironment*)0x1);
    subghz_protocol_decoder_base_set_decoder_callback(
        (SubGhzProtocolDecoderBase*)dec, rx_cb, &hf);
    for(int i = 0; i < g_n; i++)
        kia_protocol_v3_v4.decoder->feed(dec, g_durs[i] > 0, (uint32_t)(g_durs[i] < 0 ? -g_durs[i] : g_durs[i]));

    FuriString* out = furi_string_alloc();
    kia_protocol_v3_v4.decoder->get_string(dec, out);
    printf("[decode] callback hits=%d\n[decode] get_string:\n%s\n", hf.hit, furi_string_get_cstr(out));
    if(hf.hit < 1) {
        fprintf(stderr, "FAIL: canonical-key frame did NOT decode (try-all broken)\n");
        fails++;
    } else {
        printf("[decode] PASS: A8F5DFFC-keyed frame decoded via try-all\n");
    }
    furi_string_free(out);
    kia_protocol_v3_v4.decoder->free(dec);

    printf(fails ? "\n=== RESULT: FAIL (%d) ===\n" : "\n=== RESULT: PASS ===\n", fails);
    return fails ? 1 : 0;
}
