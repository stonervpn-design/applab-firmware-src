// tests/test_fuzzer_mutate.c
#include "../components/fuzz_core/fuzzer_mutate.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FuzzConfig proto_cfg(FuzzStrategy s) {
    FuzzConfig c; memset(&c, 0, sizeof c);
    c.mode = FuzzModeProtocol; c.strategy = s; c.seed = 1;
    c.seed_nbits = 24;                 // 3-byte seed 0xAA 0x55 0x0F
    c.seed_bits[0] = 0xAA; c.seed_bits[1] = 0x55; c.seed_bits[2] = 0x0F;
    return c;
}

static void test_bitflip_sweep(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyBitFlipSweep);
    // one frame per bit
    assert(fuzzer_mutate_count(&c) == 24);
    FuzzFrame f;
    // index 0 flips bit 0 (MSB of byte 0): 0xAA -> 0x2A
    assert(fuzzer_mutate_frame(&c, 0, &f));
    assert(f.nbits == 24);
    assert(f.bits[0] == 0x2A && f.bits[1] == 0x55 && f.bits[2] == 0x0F);
    assert(f.index == 0 && f.strategy == FuzzStrategyBitFlipSweep);
    // index 23 flips last bit: 0x0F -> 0x0E
    assert(fuzzer_mutate_frame(&c, 23, &f));
    assert(f.bits[2] == 0x0E && f.bits[0] == 0xAA);
    // out of range
    assert(!fuzzer_mutate_frame(&c, 24, &f));
    printf("test_bitflip_sweep OK\n");
}

static void test_boundary(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyBoundary);
    assert(fuzzer_mutate_count(&c) == 4);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); // all zeros, same nbits
    assert(f.nbits == 24 && f.bits[0] == 0 && f.bits[1] == 0 && f.bits[2] == 0);
    fuzzer_mutate_frame(&c, 1, &f); // all ones
    assert(f.bits[0] == 0xFF && f.bits[1] == 0xFF && f.bits[2] == 0xFF);
    fuzzer_mutate_frame(&c, 2, &f); assert(f.nbits == 23); // nbits-1
    fuzzer_mutate_frame(&c, 3, &f); assert(f.nbits == 25); // nbits+1
    printf("test_boundary OK\n");
}

static void test_length(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyLength);
    c.len_min_delta = -2; c.len_max_delta = 2;      // -2..+2 => 5 frames
    assert(fuzzer_mutate_count(&c) == 5);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); assert(f.nbits == 22);
    fuzzer_mutate_frame(&c, 4, &f); assert(f.nbits == 26);
    printf("test_length OK\n");
}

static void test_random_reproducible(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyRandom);
    c.rand_iters = 50; c.rand_k = 3;
    assert(fuzzer_mutate_count(&c) == 50);
    FuzzFrame a, b;
    assert(fuzzer_mutate_frame(&c, 7, &a));
    assert(fuzzer_mutate_frame(&c, 7, &b));       // same index -> identical
    assert(memcmp(a.bits, b.bits, 3) == 0);
    FuzzFrame d; fuzzer_mutate_frame(&c, 8, &d);   // different index -> differs (very likely)
    assert(memcmp(a.bits, d.bits, 3) != 0 || a.nbits != d.nbits);
    printf("test_random_reproducible OK\n");
}

static void test_field_walk(void) {
    FuzzConfig c = proto_cfg(FuzzStrategyFieldWalk);
    c.field_off = 20; c.field_len = 4;            // low nibble of byte 2
    assert(fuzzer_mutate_count(&c) == 16);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); assert((f.bits[2] & 0x0F) == 0x0);
    fuzzer_mutate_frame(&c, 5, &f); assert((f.bits[2] & 0x0F) == 0x5);
    fuzzer_mutate_frame(&c, 15, &f); assert((f.bits[2] & 0x0F) == 0xF);
    printf("test_field_walk OK\n");
}

// --- RAW mode (I-1): strategies mutate the duration array (dur[]/ndur)
// instead of the protocol bit buffer. +value = pulse, -value = gap.
static FuzzConfig raw_cfg(FuzzStrategy s) {
    FuzzConfig c; memset(&c, 0, sizeof c);
    c.mode = FuzzModeRaw; c.strategy = s; c.seed = 1;
    c.seed_ndur = 4;
    c.seed_dur[0] = 500; c.seed_dur[1] = -300; c.seed_dur[2] = 700; c.seed_dur[3] = -200;
    return c;
}

static void test_raw_bitflip_sweep(void) {
    FuzzConfig c = raw_cfg(FuzzStrategyBitFlipSweep);
    // one frame per bit across all durations: ndur(4) * 16
    assert(fuzzer_mutate_count(&c) == 64);
    FuzzFrame f;
    // index 0 flips bit 0 (MSB) of dur[0]: 500 (0x01F4) -> 0x81F4
    assert(fuzzer_mutate_frame(&c, 0, &f));
    assert(f.ndur == 4);
    assert(f.dur[0] == (int16_t)0x81F4);
    assert(f.dur[1] == -300 && f.dur[2] == 700 && f.dur[3] == -200);
    // index 63 flips the LSB (bit 15) of dur[3]: -200 (0xFF38) -> 0xFF39 = -199
    assert(fuzzer_mutate_frame(&c, 63, &f));
    assert(f.dur[3] == -199);
    assert(f.dur[0] == 500 && f.dur[1] == -300 && f.dur[2] == 700);
    // out of range
    assert(!fuzzer_mutate_frame(&c, 64, &f));
    printf("test_raw_bitflip_sweep OK\n");
}

static void test_raw_boundary(void) {
    FuzzConfig c = raw_cfg(FuzzStrategyBoundary);
    assert(fuzzer_mutate_count(&c) == 4);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); // all durations -> small min
    assert(f.ndur == 4 && f.dur[0] == 100 && f.dur[1] == 100 && f.dur[2] == 100 && f.dur[3] == 100);
    fuzzer_mutate_frame(&c, 1, &f); // all durations -> large max
    assert(f.dur[0] == 10000 && f.dur[3] == 10000);
    fuzzer_mutate_frame(&c, 2, &f); assert(f.ndur == 3); // ndur-1
    fuzzer_mutate_frame(&c, 3, &f); assert(f.ndur == 5); // ndur+1
    printf("test_raw_boundary OK\n");
}

static void test_raw_length(void) {
    FuzzConfig c = raw_cfg(FuzzStrategyLength);
    c.len_min_delta = -1; c.len_max_delta = 2;      // -1..+2 => 4 frames
    assert(fuzzer_mutate_count(&c) == 4);
    FuzzFrame f;
    fuzzer_mutate_frame(&c, 0, &f); assert(f.ndur == 3);
    fuzzer_mutate_frame(&c, 3, &f); assert(f.ndur == 6);
    printf("test_raw_length OK\n");
}

static void test_raw_random_reproducible(void) {
    FuzzConfig c = raw_cfg(FuzzStrategyRandom);
    c.rand_iters = 50; c.rand_k = 2;
    assert(fuzzer_mutate_count(&c) == 50);
    FuzzFrame a, b;
    assert(fuzzer_mutate_frame(&c, 7, &a));
    assert(fuzzer_mutate_frame(&c, 7, &b));       // same index -> identical
    assert(memcmp(a.dur, b.dur, sizeof(int16_t) * 4) == 0);
    FuzzFrame d; fuzzer_mutate_frame(&c, 8, &d);   // different index -> differs (very likely)
    assert(memcmp(a.dur, d.dur, sizeof(int16_t) * 4) != 0);
    printf("test_raw_random_reproducible OK\n");
}

int main(void) {
    test_bitflip_sweep();
    test_boundary();
    test_length();
    test_random_reproducible();
    test_field_walk();
    test_raw_bitflip_sweep();
    test_raw_boundary();
    test_raw_length();
    test_raw_random_reproducible();
    printf("TESTS_PASS\n");
    return 0;
}
