// MazdaSiemens ENCODER predict-path proof (v2 — tests the REAL TX path).
// The Rolling-Code Analyzer Next/Burst overrides ONLY the FF Cnt field and relies
// on the encoder's deserialize to rebuild the frame for that counter. v1 wrongly
// tested mzs_predict_key (display-only); this drives the actual encoder path.
// Proves: encoder deserialize with Cnt=N yields a frame whose counter == N (serial
// preserved), for N, N+1, N+5 — so Next/Burst advance correctly (no hardcoded +1).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <core/furi_string.h>

#include "lib/subghz/protocols/mazda_siemens.c"



// Build a valid captured frame (serial[63:32] btn[31:24] cnt[23:8] chk[7:0]).
static void frame_bytes(uint32_t serial, uint8_t btn, uint16_t cnt, uint8_t out[8]) {
    out[0] = serial >> 24; out[1] = serial >> 16; out[2] = serial >> 8; out[3] = serial;
    out[4] = btn;
    out[5] = cnt >> 8; out[6] = cnt & 0xFF;
    uint8_t c = 0;
    for(int i = 0; i < 7; i++) c = (uint8_t)(c + out[i]);
    out[7] = c;
}

int main(void) {
    static char env;
    const SubGhzProtocolEncoder* enc = mazda_siemens_protocol.encoder;
    if(!enc || !enc->alloc || !enc->deserialize) {
        printf("FAIL: no encoder\n");
        return 1;
    }

    const uint32_t serial = 0x1A2B3C4D;
    const uint8_t btn = 0x11;
    const uint16_t base = 0x0010;
    uint8_t key[8];
    frame_bytes(serial, btn, base, key); // the captured frame (cnt = base)

    int fail = 0;
    uint32_t offsets[] = {0, 1, 5};
    for(unsigned i = 0; i < 3; i++) {
        uint32_t want = base + offsets[i];

        // Build the capture FF exactly like the Analyzer: Key(captured) + overridden Cnt.
        FlipperFormat* ff = flipper_format_string_alloc();
        flipper_format_write_string_cstr(ff, "Protocol", mazda_siemens_protocol.name);
        uint32_t bit = MZS_BIT_COUNT;
        flipper_format_write_uint32(ff, "Bit", &bit, 1);
        flipper_format_write_hex(ff, "Key", key, 8);
        flipper_format_write_uint32(ff, "Cnt", &want, 1);
        flipper_format_rewind(ff);

        void* inst = enc->alloc((SubGhzEnvironment*)&env);
        SubGhzProtocolStatus st = enc->deserialize(inst, ff);

        // Read back the frame the encoder actually built.
        SubGhzProtocolEncoderMazdaSiemens* e = inst;
        uint32_t got_sn = (uint32_t)(e->generic.data >> 32);
        uint16_t got_cnt = (uint16_t)((e->generic.data >> 8) & 0xFFFF);

        int ok = (st == SubGhzProtocolStatusOk) && (got_cnt == (uint16_t)want) && (got_sn == serial);
        if(!ok) fail++;
        printf(
            "[Cnt=%04X] deserialize st=%d -> frame sn=%08X cnt=%04X (want cnt=%04X) => %s\n",
            want, (int)st, got_sn, got_cnt, (uint16_t)want, ok ? "PASS" : "FAIL");

        if(enc->free) enc->free(inst);
        flipper_format_free(ff);
    }
    printf(
        "\nSUMMARY: %d/3 PASS%s\n",
        3 - fail, fail ? "  <-- encoder does NOT honor Cnt override" : " (encoder honors Cnt override)");
    return fail ? 1 : 0;
}
