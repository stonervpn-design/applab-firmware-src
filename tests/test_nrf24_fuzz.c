// tests/test_nrf24_fuzz.c
// Host unit test for the nRF24 packet fuzzer mutators (Task 7 of the
// nRF24 Red-Team Suite). Compile+run under WSL:
//   wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" &&
//     gcc -std=gnu11 -Wall -Wextra -Iapplications/main/nrf24/helpers
//     tests/test_nrf24_fuzz.c applications/main/nrf24/helpers/nrf24_fuzz.c
//     -o /tmp/tnf && /tmp/tnf'

#include "nrf24_fuzz.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_base(uint8_t* b, uint8_t len) {
    for(uint8_t i = 0; i < len; i++) b[i] = (uint8_t)(0x10 + i);
}

/* --- Determinism: same (base, mode, seed, step) -> identical out/out_len,
 * across two independent calls, for all four modes. --- */
static void test_determinism_all_modes(void) {
    uint8_t base[8];
    make_base(base, 8);
    Nrf24FuzzMode modes[4] = {N24F_BITFLIP, N24F_LENSWEEP, N24F_FIELD, N24F_RANDOM};
    for(int m = 0; m < 4; m++) {
        for(uint32_t step = 0; step < 40; step++) {
            uint8_t out1[32], out2[32], len1 = 0, len2 = 0;
            nrf24_fuzz_next(base, 8, modes[m], 0xC0FFEEu, step, out1, &len1);
            nrf24_fuzz_next(base, 8, modes[m], 0xC0FFEEu, step, out2, &len2);
            assert(len1 == len2);
            assert(memcmp(out1, out2, len1) == 0);
        }
    }
    printf("test_determinism_all_modes OK\n");
}

/* --- BITFLIP: exactly one bit differs from base; that bit is at
 * step % (base_len*8); sweeping the full range covers every bit once. --- */
static void test_bitflip_single_bit(void) {
    uint8_t base[4];
    make_base(base, 4);
    uint32_t total_bits = 4u * 8u;
    int seen[32] = {0};
    for(uint32_t step = 0; step < total_bits; step++) {
        uint8_t out[32], out_len = 0;
        nrf24_fuzz_next(base, 4, N24F_BITFLIP, 1234, step, out, &out_len);
        assert(out_len == 4);

        int diff_count = 0;
        int diff_byte = -1;
        uint8_t diff_mask = 0;
        for(uint8_t i = 0; i < 4; i++) {
            uint8_t d = out[i] ^ base[i];
            if(d != 0) {
                diff_count++;
                diff_byte = i;
                diff_mask = d;
            }
        }
        assert(diff_count == 1); /* exactly one byte differs */
        /* exactly one bit set in the differing byte */
        assert(diff_mask != 0 && (diff_mask & (diff_mask - 1)) == 0);

        uint32_t idx = step % total_bits;
        uint8_t exp_byte = (uint8_t)(idx / 8);
        uint8_t exp_bit = (uint8_t)(idx % 8);
        assert((uint8_t)diff_byte == exp_byte);
        assert(diff_mask == (uint8_t)(1u << exp_bit));

        seen[idx] = 1;
    }
    for(uint32_t i = 0; i < total_bits; i++) assert(seen[i] == 1); /* every bit hit exactly once */
    printf("test_bitflip_single_bit OK\n");
}

/* --- LENSWEEP: out_len == 1+(step%32), bounded 1..32; content from base. --- */
static void test_lensweep_bounds(void) {
    uint8_t base[5];
    make_base(base, 5);
    for(uint32_t step = 0; step < 96; step++) {
        uint8_t out[32], out_len = 0;
        nrf24_fuzz_next(base, 5, N24F_LENSWEEP, 7, step, out, &out_len);
        uint8_t expected_len = (uint8_t)(1 + (step % 32));
        assert(out_len == expected_len);
        assert(out_len >= 1 && out_len <= 32);
        for(uint8_t i = 0; i < out_len; i++) {
            assert(out[i] == base[i % 5]);
        }
    }
    printf("test_lensweep_bounds OK\n");
}

/* --- FIELD: exactly one byte differs; index/value follow the formula;
 * sweeping step 0..255 hits byte 0 values 0..255. --- */
static void test_field_cycle(void) {
    uint8_t base[3];
    make_base(base, 3);
    for(uint32_t step = 0; step < 256u * 3u; step++) {
        uint8_t out[32], out_len = 0;
        nrf24_fuzz_next(base, 3, N24F_FIELD, 99, step, out, &out_len);
        assert(out_len == 3);

        uint8_t exp_byte = (uint8_t)((step / 256) % 3);
        uint8_t exp_val = (uint8_t)(step % 256);

        int diff_count = 0;
        int diff_idx = -1;
        for(uint8_t i = 0; i < 3; i++) {
            if(out[i] != base[i]) {
                diff_count++;
                diff_idx = i;
            }
        }
        if(exp_val == base[exp_byte]) {
            /* value happens to equal the base byte: no observable diff, still valid */
            assert(diff_count == 0 || (diff_count == 1 && diff_idx == exp_byte));
        } else {
            assert(diff_count == 1);
            assert(diff_idx == exp_byte);
        }
        assert(out[exp_byte] == exp_val);
        /* every other byte matches base */
        for(uint8_t i = 0; i < 3; i++) {
            if(i != exp_byte) assert(out[i] == base[i]);
        }
    }
    /* step 0..255 sweeps byte 0 through all 256 values */
    for(uint32_t step = 0; step < 256; step++) {
        uint8_t out[32], out_len = 0;
        nrf24_fuzz_next(base, 3, N24F_FIELD, 99, step, out, &out_len);
        assert(out[0] == (uint8_t)step);
    }
    printf("test_field_cycle OK\n");
}

/* --- RANDOM: reproduces for a fixed (seed, step); a different seed gives
 * a different output. --- */
static void test_random_reproducible_and_seed_sensitive(void) {
    uint8_t base[6];
    make_base(base, 6);

    uint8_t out_a[32], out_b[32], len_a = 0, len_b = 0;
    nrf24_fuzz_next(base, 6, N24F_RANDOM, 555, 10, out_a, &len_a);
    nrf24_fuzz_next(base, 6, N24F_RANDOM, 555, 10, out_b, &len_b);
    assert(len_a == 6 && len_b == 6);
    assert(memcmp(out_a, out_b, 6) == 0); /* reproducible for fixed seed+step */

    uint8_t out_c[32], len_c = 0;
    nrf24_fuzz_next(base, 6, N24F_RANDOM, 556, 10, out_c, &len_c); /* different seed */
    assert(len_c == 6);
    assert(memcmp(out_a, out_c, 6) != 0); /* different seed -> different output */

    printf("test_random_reproducible_and_seed_sensitive OK\n");
}

/* --- Guard: base_len==0 or >32 clamps to 1..32; NULL base with RANDOM is
 * fine; NULL out/out_len is a safe no-op. --- */
static void test_guards(void) {
    uint8_t out[32], out_len = 0xAA;

    /* NULL base with N24F_RANDOM: must not crash, produces output. */
    nrf24_fuzz_next(NULL, 8, N24F_RANDOM, 1, 1, out, &out_len);
    assert(out_len == 8);

    /* NULL out / NULL out_len: safe no-op, must not crash. */
    uint8_t base[4];
    make_base(base, 4);
    nrf24_fuzz_next(base, 4, N24F_BITFLIP, 1, 1, NULL, &out_len);
    nrf24_fuzz_next(base, 4, N24F_BITFLIP, 1, 1, out, NULL);
    nrf24_fuzz_next(base, 4, N24F_BITFLIP, 1, 1, NULL, NULL);

    /* base_len == 0 clamps to 1 (no crash, out_len in range). */
    uint8_t out_len2 = 0;
    nrf24_fuzz_next(base, 0, N24F_BITFLIP, 1, 1, out, &out_len2);
    assert(out_len2 >= 1 && out_len2 <= 32);

    /* base_len > 32 clamps to 32 (no crash, out_len in range). */
    uint8_t big_base[40];
    memset(big_base, 0x55, sizeof big_base);
    uint8_t out_len3 = 0;
    nrf24_fuzz_next(big_base, 200, N24F_FIELD, 1, 1, out, &out_len3);
    assert(out_len3 >= 1 && out_len3 <= 32);

    printf("test_guards OK\n");
}

int main(void) {
    test_determinism_all_modes();
    test_bitflip_single_bit();
    test_lensweep_bounds();
    test_field_cycle();
    test_random_reproducible_and_seed_sensitive();
    test_guards();
    printf("test_nrf24_fuzz: ALL PASS\n");
    return 0;
}
