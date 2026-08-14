// Host test for lib/subghz/protocols/land_rover_rke.c (plan B, Task 1).
// Synthesizes a RAW duration stream for a known (serial, button, hop) frame
// per the KAT src/protocols/land_rover_rke.rs layout, feeds it through the
// REAL decoder's feed(), and asserts the decoded generic.serial/btn match.
// Build (mirrors build_lib.sh's recipe, target swapped to this test):
//   wsl bash -lc 'cd <tree> && \
//     gcc -std=gnu11 -g -O1 -w \
//       -Itools/decoder_host_test/shim -I. \
//       -Icomponents/flipper_format -Icomponents/toolbox -Icomponents/toolbox/stream \
//       -Icomponents/furi -Icomponents/furi/core -Icomponents/mlib -Icomponents \
//       -Ilib -Iapplications/main/subghz/helpers \
//       -D_ATTRIBUTE\(x\)=__attribute__\(x\) \
//       tools/decoder_host_test/test_land_rover_rke.c \
//       lib/subghz/protocols/land_rover_rke.c lib/subghz/protocols/protocols_common.c \
//       components/furi/core/furi_string.c components/flipper_format/flipper_format.c \
//       components/flipper_format/flipper_format_stream.c components/toolbox/stream/stream.c \
//       components/toolbox/stream/string_stream.c components/toolbox/manchester_decoder.c \
//       components/toolbox/hex.c components/toolbox/strint.c \
//       lib/subghz/blocks/decoder.c lib/subghz/blocks/generic.c lib/subghz/blocks/math.c \
//       lib/subghz/blocks/encoder.c lib/subghz/blocks/custom_btn.c lib/subghz/protocols/base.c \
//       tools/decoder_host_test/host_shim.c \
//       -o tools/decoder_host_test/test_land_rover_rke && \
//     ./tools/decoder_host_test/test_land_rover_rke'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>

extern const SubGhzProtocol land_rover_rke_protocol;

// ---- known test frame ----
#define TEST_SERIAL 0x123456UL
#define TEST_BUTTON 0x2UL // Unlock
#define TEST_HOP    0xDEADBEEFUL
#define TEST_FLAGS  0x0UL
#define TEST_STATUS 0x0UL

#define PREAMBLE_PAIRS 18U // within the spec's [16,20] range

typedef struct {
    int hit;
} HitFlag;

static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

// Returns the value of frame bit `i` (0 = MSB = V[65], 65 = LSB = V[0]).
static bool frame_bit(uint8_t extra, uint64_t data, int i) {
    if(i == 0) return (extra >> 1) & 1U;
    if(i == 1) return extra & 1U;
    const int shift = 63 - (i - 2);
    return (data >> shift) & 1ULL;
}

// Feed one duration into the decoder's RAW stream (durs/n/cap by ref).
static void emit(int* durs, size_t* n, size_t cap, bool level, uint32_t us) {
    if(*n >= cap) {
        fprintf(stderr, "emit(): RAW buffer overflow\n");
        exit(1);
    }
    durs[(*n)++] = level ? (int)us : -(int)us;
}

int main(void) {
    // Build the 66-bit frame: extra_bits = V[65:64], data = V[63:0].
    const uint32_t hop = (uint32_t)TEST_HOP;
    const uint64_t data = ((uint64_t)(hop & 0x3FFFFFFFULL) << 34) | (TEST_SERIAL << 10) |
                           (TEST_BUTTON << 6) | (TEST_FLAGS << 2) | TEST_STATUS;
    const uint8_t extra = (uint8_t)((hop >> 30) & 0x3U);

    // ---- synthesize the RAW duration stream ----
    static int durs[4096];
    size_t n = 0;

    for(uint32_t i = 0; i < PREAMBLE_PAIRS; i++) {
        emit(durs, &n, 4096, true, 400);
        emit(durs, &n, 4096, false, 600);
    }
    // Sync: 400us HIGH (same shape as a preamble pulse) + 9600us LOW (discriminator).
    emit(durs, &n, 4096, true, 400);
    emit(durs, &n, 4096, false, 9600);

    for(int i = 0; i < 66; i++) {
        const bool bit = frame_bit(extra, data, i);
        if(bit) {
            emit(durs, &n, 4096, true, 700);
            emit(durs, &n, 4096, false, 300);
        } else {
            emit(durs, &n, 4096, true, 300);
            emit(durs, &n, 4096, false, 700);
        }
    }

    // ---- feed through the REAL decoder ----
    static char env;
    void* dec = land_rover_rke_protocol.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);

    for(size_t i = 0; i < n; i++) {
        int v = durs[i];
        land_rover_rke_protocol.decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }

    if(hf.hit == 0) {
        fprintf(stderr, "FAIL: decoder did not fire (hits=0)\n");
        return 1;
    }

    FuriString* out = furi_string_alloc();
    land_rover_rke_protocol.decoder->get_string(dec, out);
    const char* str = furi_string_get_cstr(out);
    printf("get_string() output:\n%s\n", str);

    unsigned long got_serial = 0, got_btn = 0, got_hop = 0;
    // "Sn:%06lX  Btn:%01X - ..." / "Hop(raw,ENC):%08lX"
    if(sscanf(str, "%*[^\n]\nSn:%lX  Btn:%lX", &got_serial, &got_btn) != 2) {
        fprintf(stderr, "FAIL: could not parse Sn/Btn from get_string() output\n");
        furi_string_free(out);
        land_rover_rke_protocol.decoder->free(dec);
        return 1;
    }
    {
        const char* hop_field = strstr(str, "Hop(raw,ENC):");
        if(hop_field) sscanf(hop_field, "Hop(raw,ENC):%lX", &got_hop);
    }

    printf(
        "parsed: serial=0x%06lX btn=0x%lX hop=0x%08lX\n",
        got_serial,
        got_btn,
        got_hop);

    bool ok = true;
    if(got_serial != TEST_SERIAL) {
        fprintf(
            stderr,
            "FAIL: serial mismatch: got 0x%06lX expected 0x%06lX\n",
            got_serial,
            (unsigned long)TEST_SERIAL);
        ok = false;
    }
    if(got_btn != TEST_BUTTON) {
        fprintf(
            stderr,
            "FAIL: button mismatch: got 0x%lX expected 0x%lX\n",
            got_btn,
            (unsigned long)TEST_BUTTON);
        ok = false;
    }
    if(got_hop != TEST_HOP) {
        fprintf(
            stderr,
            "FAIL: hop mismatch: got 0x%08lX expected 0x%08lX (raw passthrough must be exact)\n",
            got_hop,
            (unsigned long)TEST_HOP);
        ok = false;
    }

    furi_string_free(out);
    land_rover_rke_protocol.decoder->free(dec);

    if(!ok) return 1;

    printf(
        "PASS: land_rover_rke decoded serial=0x%06lX btn=0x%lX(Unlock) hop=0x%08lX "
        "(hits=%d)\n",
        got_serial,
        got_btn,
        got_hop,
        hf.hit);
    return 0;
}
