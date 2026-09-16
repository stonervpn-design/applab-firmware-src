// Host proof of the Genie sequence-store pure core (the .gne format + append/cursor logic).
// Compiles ONLY the core (GENIE_SEQ_HOST_TEST gates out the storage wrappers).
// Build+run: bash tools/decoder_host_test/build_test_genie_seq.sh
#define GENIE_SEQ_HOST_TEST
#include "../../applications/main/subghz/helpers/genie_seq.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint8_t* buf = malloc(GENIE_SEQ_FILE_SIZE);
    assert(buf);

    // header + big-endian stamping
    genie_seq_buf_init(buf, 0x007F1991u);
    assert(genie_seq_buf_valid(buf));
    assert(genie_seq_buf_sn(buf) == 0x007F1991u);
    assert(genie_seq_buf_rec_count(buf) == 0);
    assert(genie_seq_buf_last_sent(buf) == 0);
    assert(buf[0] == 0x47 && buf[1] == 0x2A); // magic BE
    assert(buf[2] == 0x02 && buf[3] == 0x05); // version 2.5 BE

    // append + read-back (records stored big-endian)
    assert(genie_seq_buf_append(buf, 0x9400A98Fu) == 1);
    assert(genie_seq_buf_append(buf, 0x11223344u) == 2);
    assert(genie_seq_buf_append(buf, 0xAABBCCDDu) == 3);
    assert(genie_seq_buf_rec_count(buf) == 3);
    assert(genie_seq_buf_code_at(buf, 0) == 0x9400A98Fu);
    assert(genie_seq_buf_code_at(buf, 2) == 0xAABBCCDDu);
    assert(buf[16] == 0x94 && buf[17] == 0x00 && buf[18] == 0xA9 && buf[19] == 0x8F);

    // resume slot-guard: rewind rec_count; a DIFFERENT code at the occupied slot is rejected
    // without corrupting it; re-appending the SAME code proceeds.
    buf[10] = 0;
    buf[11] = 0; // rec_count = 0
    assert(genie_seq_buf_append(buf, 0xDEADBEEFu) == 0); // slot0 holds a different code -> no write
    assert(genie_seq_buf_rec_count(buf) == 0);
    assert(genie_seq_buf_code_at(buf, 0) == 0x9400A98Fu);
    assert(genie_seq_buf_append(buf, 0x9400A98Fu) == 1); // same code -> proceeds
    assert(genie_seq_buf_rec_count(buf) == 1);

    // cursor
    genie_seq_buf_set_last_sent(buf, 12345);
    assert(genie_seq_buf_last_sent(buf) == 12345);
    assert(buf[8] == 0x30 && buf[9] == 0x39); // 12345 = 0x3039 BE

    // full table: at 0xFFFF, append writes the last slot but returns FULL and never increments
    buf[10] = 0xFF;
    buf[11] = 0xFF;
    assert(genie_seq_buf_append(buf, 0x12345678u) == GENIE_SEQ_FULL);
    assert(genie_seq_buf_code_at(buf, 0xFFFF) == 0x12345678u);
    assert(genie_seq_buf_rec_count(buf) == 0xFFFF);

    // version reject (major > 2)
    genie_seq_buf_init(buf, 0x1234u);
    buf[2] = 0x03;
    assert(!genie_seq_buf_valid(buf));

    // exact ARF-bundled 007F1991.gne header parses identically
    memset(buf, 0, GENIE_SEQ_FILE_SIZE);
    uint8_t sample[20] = {0x47, 0x2A, 0x02, 0x05, 0x00, 0x7F, 0x19, 0x91, 0x00, 0x05,
                          0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x94, 0x00, 0xA9, 0x8F};
    memcpy(buf, sample, 20);
    assert(genie_seq_buf_valid(buf));
    assert(genie_seq_buf_sn(buf) == 0x007F1991u);
    assert(genie_seq_buf_last_sent(buf) == 0x0005u);
    assert(genie_seq_buf_rec_count(buf) == 0xFFFFu);
    assert(genie_seq_buf_code_at(buf, 0) == 0x9400A98Fu);

    printf("test_genie_seq OK: header/append/big-endian/resume-guard/full/version/ARF-sample\n");
    free(buf);
    return 0;
}
