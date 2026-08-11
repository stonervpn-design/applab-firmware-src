#include "fuzzer_frame.h"
#include <stdio.h>
#include <string.h>

int fuzzer_frame_build(const FuzzFrame* f, FuzzMode mode, const char* protocol,
                       uint32_t te, uint8_t repeat, char* out, int cap) {
    int n = 0;
    if(mode == FuzzModeProtocol) {
        // Key is 8 bytes, right-justified: pad high bytes with 00.
        uint8_t key[8]; memset(key, 0, sizeof key);
        int nb = (f->nbits + 7) / 8; if(nb > 8) nb = 8;
        for(int i = 0; i < nb; i++) key[8 - nb + i] = f->bits[i];
        n = snprintf(out, cap,
            "Protocol: %s\nBit: %u\nKey: %02X %02X %02X %02X %02X %02X %02X %02X\nTE: %lu\nRepeat: %u\n",
            protocol ? protocol : "Princeton", (unsigned)f->nbits,
            key[0],key[1],key[2],key[3],key[4],key[5],key[6],key[7],
            (unsigned long)te, (unsigned)repeat);
    } else {
        n = snprintf(out, cap, "Protocol: RAW\nRAW_Data:");
        for(uint16_t i = 0; i < f->ndur && n > 0 && n < cap; i++)
            n += snprintf(out + n, cap - n, " %d", (int)f->dur[i]);
        if(n > 0 && n < cap) n += snprintf(out + n, cap - n, "\n");
    }
    return (n > 0 && n < cap) ? n : -1;
}
