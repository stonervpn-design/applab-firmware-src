/*
 * Host round-trip proof for applications/main/subghz/helpers/keeloq_instant.c
 *
 * Proves: keeloq_instant_synthesize() produces a fix/hop pair that a real
 * KeeLoq receiver (i.e. this codebase's own decoder,
 * lib/subghz/protocols/keeloq.c subghz_protocol_keeloq_check_decrypt())
 * would accept -- by independently re-deriving the device key the same way
 * the encoder does, decrypting the synthesized hop with the REAL
 * subghz_protocol_keeloq_common_decrypt(), and checking the same
 * (btn, discriminator, counter) relationship the decoder checks:
 *   lib/subghz/protocols/keeloq.c:1100-1108
 *     (decrypt >> 28 == btn) &&
 *     (((decrypt >> 16) & 0xFF) == end_serial || == 0)
 *     end_serial = (uint16_t)(fix & 0xFF)      [keeloq.c:1160]
 *     instance->cnt = decrypt & 0x0000FFFF      [keeloq.c:1108]
 *
 * Build/run: `make test` (see Makefile for the keeloq_common.c staging).
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../../applications/main/subghz/helpers/keeloq_instant.h"

/* Same technique as keeloq_instant.c: local ABI-compatible prototypes for
 * the REAL primitives in the staged keeloq_common.c, so the test can
 * independently re-derive/decrypt without including the heavy header. */
extern uint32_t subghz_protocol_keeloq_common_decrypt(const uint32_t data, const uint64_t key);
extern uint64_t subghz_protocol_keeloq_common_normal_learning(uint32_t data, const uint64_t key);

#define KEELOQ_LEARNING_SECURE 3u /* mirrors keeloq_common.h; used only to prove it's rejected */

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if(!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                    \
        }                                                                    \
    } while(0)

/* Independently re-derive the device key the same way the encoder
 * (keeloq.c) does, for the two learning types under test. Deliberately
 * duplicated (not shared) from keeloq_instant.c so this test doesn't just
 * echo the implementation back at itself. */
static uint64_t reference_device_key(uint64_t mf_key, uint8_t learning_type, uint32_t fix) {
    if(learning_type == KEELOQ_INSTANT_LEARNING_SIMPLE) {
        return mf_key;
    } else if(learning_type == KEELOQ_INSTANT_LEARNING_NORMAL) {
        return subghz_protocol_keeloq_common_normal_learning(fix, mf_key);
    }
    fprintf(stderr, "reference_device_key: unhandled learning_type %u\n", learning_type);
    abort();
}

/* Runs the full round trip for one (mf_key, serial, btn, counter, learning_type)
 * and asserts the decoder-side acceptance relationship holds. */
static void assert_round_trip(
    uint64_t mf_key,
    uint8_t learning_type,
    uint32_t serial,
    uint8_t btn,
    uint16_t counter) {
    uint32_t fix = 0, hop = 0;
    bool ok = keeloq_instant_synthesize(mf_key, learning_type, serial, btn, counter, &fix, &hop);
    CHECK(ok);
    if(!ok) return;

    uint32_t expected_fix = ((uint32_t)btn & 0xF) << 28 | (serial & 0x0FFFFFFF);
    CHECK(fix == expected_fix);

    uint64_t device_key = reference_device_key(mf_key, learning_type, fix);
    uint32_t decrypt = subghz_protocol_keeloq_common_decrypt(hop, device_key);

    uint16_t end_serial = (uint16_t)(fix & 0xFF);
    uint8_t decoded_btn = (uint8_t)(decrypt >> 28);
    uint16_t decoded_disc = (uint16_t)((decrypt >> 16) & 0xFF);
    uint16_t decoded_cnt = (uint16_t)(decrypt & 0x0000FFFF);

    CHECK(decoded_btn == (btn & 0xF));
    CHECK(decoded_disc == end_serial || decoded_disc == 0);
    CHECK(decoded_cnt == counter);
}

int main(void) {
    /* --- SIMPLE learning round trip --- */
    assert_round_trip(0x0123456789ABCDEFULL, KEELOQ_INSTANT_LEARNING_SIMPLE, 0x0A3C9F1u, 0x2u, 100);
    assert_round_trip(0xFEDCBA9876543210ULL, KEELOQ_INSTANT_LEARNING_SIMPLE, 0x0FFFFFFu, 0xFu, 0);
    assert_round_trip(0xDEADBEEFCAFEBABEULL, KEELOQ_INSTANT_LEARNING_SIMPLE, 0x0000001u, 0x1u, 0xFFFF);

    /* --- NORMAL learning round trip (device key derived per-serial) --- */
    assert_round_trip(0x0123456789ABCDEFULL, KEELOQ_INSTANT_LEARNING_NORMAL, 0x0A3C9F1u, 0x2u, 100);
    assert_round_trip(0xFEDCBA9876543210ULL, KEELOQ_INSTANT_LEARNING_NORMAL, 0x0FFFFFFu, 0xFu, 500);
    assert_round_trip(0x1122334455667788ULL, KEELOQ_INSTANT_LEARNING_NORMAL, 0x0BEEF00u, 0x4u, 12345);

    /* --- counter+1 / counter+2 produce distinct, both-valid hops (this is
     * the actual "instant unlock" primitive: two candidate future frames) --- */
    {
        uint64_t mf_key = 0x0123456789ABCDEFULL;
        uint32_t serial = 0x0A3C9F1u;
        uint8_t btn = 0x2u;
        uint16_t base_counter = 1000;

        uint32_t fix0 = 0, hop0 = 0, fix1 = 0, hop1 = 0, fix2 = 0, hop2 = 0;
        CHECK(keeloq_instant_synthesize(
            mf_key, KEELOQ_INSTANT_LEARNING_NORMAL, serial, btn, base_counter, &fix0, &hop0));
        CHECK(keeloq_instant_synthesize(
            mf_key,
            KEELOQ_INSTANT_LEARNING_NORMAL,
            serial,
            btn,
            (uint16_t)(base_counter + 1),
            &fix1,
            &hop1));
        CHECK(keeloq_instant_synthesize(
            mf_key,
            KEELOQ_INSTANT_LEARNING_NORMAL,
            serial,
            btn,
            (uint16_t)(base_counter + 2),
            &fix2,
            &hop2));

        CHECK(fix0 == fix1 && fix1 == fix2); /* fix is counter-independent */
        CHECK(hop0 != hop1);
        CHECK(hop1 != hop2);
        CHECK(hop0 != hop2);

        assert_round_trip(
            mf_key, KEELOQ_INSTANT_LEARNING_NORMAL, serial, btn, (uint16_t)(base_counter + 1));
        assert_round_trip(
            mf_key, KEELOQ_INSTANT_LEARNING_NORMAL, serial, btn, (uint16_t)(base_counter + 2));
    }

    /* --- unsupported learning type (SECURE needs a `seed` this single-frame
     * attack never has) must be honestly rejected, not silently wrong --- */
    {
        uint32_t fix = 0, hop = 0;
        bool ok = keeloq_instant_synthesize(
            0x0123456789ABCDEFULL, KEELOQ_LEARNING_SECURE, 0x0A3C9F1u, 0x2u, 100, &fix, &hop);
        CHECK(!ok);
    }

    /* --- serial/btn masking: caller passing an over-wide serial/btn must
     * not corrupt the other field --- */
    {
        uint32_t fix = 0, hop = 0;
        bool ok = keeloq_instant_synthesize(
            0x0123456789ABCDEFULL,
            KEELOQ_INSTANT_LEARNING_SIMPLE,
            0xFFFFFFFFu /* only low 28 bits are serial */,
            0xFFu /* only low 4 bits are btn */,
            42,
            &fix,
            &hop);
        CHECK(ok);
        CHECK(fix == 0xFFFFFFFFu); /* (0xF<<28)|(0xFFFFFFF) both mask to all-ones here */
        uint32_t fix2 = 0, hop2 = 0;
        keeloq_instant_synthesize(
            0x0123456789ABCDEFULL, KEELOQ_INSTANT_LEARNING_SIMPLE, 0x0FFFFFFFu, 0x0Fu, 42, &fix2, &hop2);
        CHECK(fix == fix2);
        CHECK(hop == hop2);
    }

    if(g_failures == 0) {
        printf("ALL PASS (keeloq_instant round-trip)\n");
        return 0;
    } else {
        fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
}
