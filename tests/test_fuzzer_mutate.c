// tests/test_fuzzer_mutate.c
#include "../applications/main/subghz/helpers/fuzzer_mutate.c"
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

int main(void) {
    test_bitflip_sweep();
    printf("TESTS_PASS\n");
    return 0;
}
