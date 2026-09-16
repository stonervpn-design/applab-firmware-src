// Mazda V0 keyless round-trip proof (mirrors test_ford_v0_rt.c).
// Includes the REAL firmware mazda_v0.c to reach its static encode/decode/predict
// (all keyless: serial+button+20-bit cleartext counter + additive checksum +
// reversible interleave + forward XOR mask; NO manufacturer secret). Proves:
//   decode(encode(serial,btn,cnt))   == (serial,btn,cnt)      [self-consistent]
//   decode(encode(serial,btn,cnt+1)) == (serial,btn,cnt+1)    [bumped counter]
//   encode(cnt) != encode(cnt+1)                              [real synthesis, not replay]
//   predict_key == encode_key                                 [predict path === encoder path]
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>

#include "lib/subghz/protocols/mazda_v0.c"

int main(void) {
    struct { uint32_t serial; uint8_t btn; uint32_t cnt; } cases[] = {
        {0x1A2B3C4D, 0x1, 0x00010},
        {0xDEADBEEF, 0x2, 0x3FABC},
        {0x00012345, 0x4, 0xFFFFE}, // near 20-bit wrap
        {0xFFFFFFFF, 0x8, 0x00000},
    };
    int fail = 0, total = 0;
    for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t sn = cases[i].serial, bc = cases[i].cnt;
        uint8_t bt = cases[i].btn;
        uint64_t base_frame = mazda_v0_encode_key(sn, bt, bc);
        for(int step = 0; step <= 1; step++) {
            uint32_t cnt = (bc + step) & 0xFFFFF;
            uint64_t frame = mazda_v0_encode_key(sn, bt, cnt);
            uint64_t pframe = mazda_v0_predict_key(sn, bt, cnt);

            SubGhzBlockGeneric g;
            memset(&g, 0, sizeof(g));
            g.data = frame;
            mazda_v0_decode_key(&g);

            int synth_ok = (step == 0) || (frame != base_frame); // cnt+1 must differ
            int ok = (g.serial == sn) && ((uint8_t)g.btn == bt) && (g.cnt == cnt) &&
                     (pframe == frame) && synth_ok;
            total++;
            if(!ok) fail++;
            printf(
                "[%s] sn=%08X btn=%X cnt=%05X -> key=%016llX | dec sn=%08X btn=%X cnt=%05X "
                "predict=%s synth=%s => %s\n",
                step ? "cnt+1" : "base ",
                sn, bt, cnt, (unsigned long long)frame,
                g.serial, (uint8_t)g.btn, g.cnt,
                (pframe == frame) ? "OK" : "BAD",
                synth_ok ? "OK" : "SAME!",
                ok ? "PASS" : "FAIL");
        }
    }
    printf(
        "\nSUMMARY: %d/%d PASS%s\n",
        total - fail, total, fail ? "  <-- FAILURES" : " (keyless round-trip proven)");
    return fail ? 1 : 0;
}
