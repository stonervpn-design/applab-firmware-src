// Proves the REAL firmware AERF crypto (lib/subghz/protocols/keeloq_common.c) round-trips:
//   (1) encrypt_derived is the exact inverse of decrypt_derived, and
//   (2) the full AERF hop path keeloq.c uses — man = learning_aerf(fix,K);
//       hop = encrypt_derived(plaintext, man, 0x210)  [encode, keeloq.c:499]
//       dec = decrypt_derived(hop, man, 0x240)         [decode, keeloq.c:1338]
//       => dec == plaintext.
// Build+run: bash tools/decoder_host_test/build_test_keeloq_aerf_roundtrip.sh
#include <lib/subghz/protocols/keeloq_common.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t st = 0x2468ace0u;
static uint32_t xr(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

int main(void) {
    const long N = 3000000;

    // (1) encrypt_derived == inverse of decrypt_derived, over the round counts keeloq.c uses.
    uint32_t limits[] = {0x210u, 0x240u};
    for(int li = 0; li < 2; li++) {
        uint32_t L = limits[li];
        for(long i = 0; i < N; i++) {
            uint32_t h = xr();
            uint64_t key = ((uint64_t)xr() << 32) | xr();
            uint32_t p = subghz_protocol_keeloq_common_decrypt_derived(h, key, L);
            uint32_t h2 = subghz_protocol_keeloq_common_encrypt_derived(p, key, L);
            if(h2 != h) {
                printf("FAIL inverse L=%X i=%ld h=%08X p=%08X got=%08X\n", L, i, h, p, h2);
                return 1;
            }
        }
        printf("OK real decrypt_derived/encrypt_derived inverse @L=0x%03X : %ld vectors\n", L, N);
    }

    // (2) Full AERF hop encode->decode identity, exactly as keeloq.c wires it: encode at
    // 0x210, decode at 0x240 (both cap to 0x210 active rounds), device key via learning_aerf.
    for(long i = 0; i < N; i++) {
        uint32_t serial = xr() & 0x0FFFFFFFu;
        uint8_t btn = (uint8_t)(xr() & 0xF);
        uint16_t cnt = (uint16_t)xr();
        uint64_t K = ((uint64_t)xr() << 32) | xr();
        uint32_t fix = ((uint32_t)btn << 28) | serial;
        uint32_t plain = ((uint32_t)btn << 28) | ((serial & 0x3FFu) << 16) | cnt; // 0xBSSSCCCC

        uint64_t man_enc = subghz_protocol_keeloq_common_learning_aerf(fix, K);
        uint32_t hop = subghz_protocol_keeloq_common_encrypt_derived(plain, man_enc, 0x210u);

        uint64_t man_dec = subghz_protocol_keeloq_common_learning_aerf(fix, K);
        uint32_t dec = subghz_protocol_keeloq_common_decrypt_derived(hop, man_dec, 0x240u);

        if(dec != plain) {
            printf("FAIL AERF hop rt i=%ld plain=%08X hop=%08X dec=%08X\n", i, plain, hop, dec);
            return 1;
        }
    }
    printf("OK AERF hop encode->decode identity (learning_aerf + encrypt_derived): %ld vectors\n", N);
    printf("ALL AERF FIRMWARE-SOURCE ROUND-TRIP TESTS PASSED\n");
    return 0;
}
