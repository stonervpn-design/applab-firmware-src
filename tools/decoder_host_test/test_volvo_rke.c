// Host test for lib/subghz/protocols/volvo_rke.c.
//
// The real Volvo RKE payload bit-offsets are NOT public (Olsson 2013 gives the
// framing envelope only), so this test does NOT claim real offsets. It defines
// a SELF-CONSISTENT synthetic frame layout, FSK+Manchester-encodes two presses
// at the thesis timing (5.76 kbit/s -> te=87us half-symbol) with a DIFFERENT
// wake-up lead-in count each press, feeds them through the REAL decoder's
// feed(), and asserts:
//   1. both presses fire the decoder callback,
//   2. the tail-aligned 128-bit window demodulates BIT-EXACTLY (proving the
//      Manchester demod + tail window survive the differing lead-in counts),
//   3. the two-frame stability diff separates constant (marker+ID+button+high
//      counter bits) from volatile (low counter bit + AES) correctly,
//   4. the candidate-AES 64-bit window heuristic locks onto the AES region.
//
// Synthetic layout (MSB=bit127): [marker:16][ID:32][button:8][counter:8][AES:64]
//
// Build (mirrors build_lib.sh's recipe):
//   wsl bash -lc 'cd <tree> && bash tools/decoder_host_test/build_test_volvo_rke.sh'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>
#include <core/furi_string.h>

extern const SubGhzProtocol volvo_rke_protocol;

#define TE 87U // half-symbol microseconds

typedef struct {
    int hit;
} HitFlag;

static void rx_cb(SubGhzProtocolDecoderBase* base, void* ctx) {
    (void)base;
    ((HitFlag*)ctx)->hit++;
}

// ---- half-symbol level stream, coalesced into RAW pulses ----
static int durs[8192];
static size_t ndur = 0;
static bool have_run = false;
static bool run_level = false;
static uint32_t run_us = 0;

static void flush_run(void) {
    if(!have_run) return;
    if(ndur >= 8192) {
        fprintf(stderr, "durs overflow\n");
        exit(1);
    }
    durs[ndur++] = run_level ? (int)run_us : -(int)run_us;
    have_run = false;
    run_us = 0;
}

// Emit one half-symbol of `level`, coalescing equal-level runs (like a real RAW
// capture). Manchester guarantees runs are at most 2 half-symbols (<=2*te).
static void half(bool level) {
    if(have_run && run_level == level) {
        run_us += TE;
    } else {
        flush_run();
        have_run = true;
        run_level = level;
        run_us = TE;
    }
}

// Manchester bit (rtl_433 zerobit / 802.3): 1 -> LOW then HIGH; 0 -> HIGH then LOW.
static void man_bit(bool b) {
    half(!b);
    half(b);
}

static void gap(uint32_t us) {
    flush_run();
    if(ndur >= 8192) exit(1);
    durs[ndur++] = -(int)us; // long LOW -> decoder finalizes the burst
}

// Encode a 128-bit frame (hi:lo) MSB-first after `leadin` Manchester "1" bits.
static void encode_press(uint32_t leadin, uint64_t hi, uint64_t lo) {
    for(uint32_t i = 0; i < leadin; i++) man_bit(true); // wake-up / preamble clock
    for(int i = 127; i >= 0; i--) {
        bool b = (i >= 64) ? (((hi >> (i - 64)) & 1U) != 0U) : (((lo >> i) & 1U) != 0U);
        man_bit(b);
    }
    flush_run();
    gap(10000); // > 3*te -> finalize this burst (keeps the session diff)
}

// Grab the four 8-hex-digit groups after "Frame:" (hi hi lo lo).
static int parse_frame(const char* s, uint64_t* hi, uint64_t* lo) {
    const char* p = strstr(s, "Frame:");
    if(!p) return 0;
    p += 6;
    unsigned long a, b, c, d;
    if(sscanf(p, "%8lx%8lx", &a, &b) != 2) return 0;
    const char* nl = strchr(p, '\n');
    if(!nl) return 0;
    // second line begins with spaces then the lo hex
    while(*nl == '\n' || *nl == '\r' || *nl == ' ') nl++;
    if(sscanf(nl, "%8lx%8lx", &c, &d) != 2) return 0;
    *hi = ((uint64_t)a << 32) | b;
    *lo = ((uint64_t)c << 32) | d;
    return 1;
}

static int parse_u(const char* s, const char* key, unsigned* out) {
    const char* p = strstr(s, key);
    if(!p) return 0;
    p += strlen(key);
    return sscanf(p, "%u", out) == 1;
}

int main(void) {
    const uint16_t marker = 0xA5A5;
    const uint32_t id = 0x0BADF00DUL;
    const uint8_t button = 0x02; // Unlock
    const uint8_t cnt1 = 0x10, cnt2 = 0x11; // +1 -> exactly one flipped bit
    const uint64_t aes1 = 0x0123456789ABCDEFULL;
    const uint64_t aes2 = 0xFEDCBA9876543210ULL; // XOR aes1 = all-ones -> 64 flips

    const uint64_t hi1 =
        ((uint64_t)marker << 48) | ((uint64_t)id << 16) | ((uint64_t)button << 8) | cnt1;
    const uint64_t hi2 =
        ((uint64_t)marker << 48) | ((uint64_t)id << 16) | ((uint64_t)button << 8) | cnt2;

    // Two presses with DIFFERENT lead-in counts (30 vs 45) -> proves tail
    // alignment is robust to a varying wake-up length.
    encode_press(30, hi1, aes1);
    encode_press(45, hi2, aes2);

    static char env;
    void* dec = volvo_rke_protocol.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    HitFlag hf = {0};
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &hf);

    for(size_t i = 0; i < ndur; i++) {
        int v = durs[i];
        volvo_rke_protocol.decoder->feed(dec, v > 0, (uint32_t)(v < 0 ? -v : v));
    }

    int fail = 0;

    if(hf.hit < 2) {
        fprintf(stderr, "FAIL: expected >=2 callback fires, got %d\n", hf.hit);
        fail = 1;
    }

    FuriString* out = furi_string_alloc();
    volvo_rke_protocol.decoder->get_string(dec, out);
    const char* str = furi_string_get_cstr(out);
    printf("get_string() after press 2:\n%s\n", str);

    uint64_t ghi = 0, glo = 0;
    if(!parse_frame(str, &ghi, &glo)) {
        fprintf(stderr, "FAIL: could not parse Frame hex\n");
        fail = 1;
    } else {
        if(ghi != hi2 || glo != aes2) {
            fprintf(
                stderr,
                "FAIL: tail-window mismatch\n  got  hi=%016llX lo=%016llX\n  want hi=%016llX lo=%016llX\n",
                (unsigned long long)ghi,
                (unsigned long long)glo,
                (unsigned long long)hi2,
                (unsigned long long)aes2);
            fail = 1;
        } else {
            printf("PASS: tail window bit-exact despite differing lead-in\n");
        }
    }

    unsigned constb = 0, chgb = 0, winoff = 999, flips = 0;
    parse_u(str, "Const:", &constb);
    parse_u(str, "Chg:", &chgb);
    parse_u(str, "win@", &winoff);
    parse_u(str, "flips:", &flips);

    // Expected: counter 0x10^0x11 = 1 flip; AES XOR = 64 flips -> changed = 65,
    // constant = 128 - 65 = 63.
    if(chgb != 65) {
        fprintf(stderr, "FAIL: changed bits = %u, want 65\n", chgb);
        fail = 1;
    }
    if(constb != 63) {
        fprintf(stderr, "FAIL: constant bits = %u, want 63\n", constb);
        fail = 1;
    }
    // AES window: the 64-bit region [63:0] is fully flipped -> offset 0, 64 flips.
    if(winoff != 0 || flips != 64) {
        fprintf(stderr, "FAIL: AES window off=%u flips=%u, want off=0 flips=64\n", winoff, flips);
        fail = 1;
    }
    if(!fail) printf("PASS: stability diff + AES window heuristic correct\n");

    furi_string_free(out);
    volvo_rke_protocol.decoder->free(dec);

    printf(fail ? "\nTEST: FAIL\n" : "\nTEST: PASS\n");
    return fail;
}
