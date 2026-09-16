// Host feeder: run REAL .sub RAW_Data captures through the real volvo_rke
// decoder to reproduce / verify false-positive behaviour.
//   argv[1..] = paths to Flipper RAW .sub files.
// Prints, per file, how many times the decoder fired "found" and the decoded
// get_string() of the last fire. Exit 0 always (diagnostic tool).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <core/furi_string.h>

extern const SubGhzProtocol volvo_rke_protocol;

typedef struct {
    int hit;
    void* dec;
    char last[4096];
} Ctx;

static void rx_cb(SubGhzProtocolDecoderBase* base, void* c) {
    (void)base;
    Ctx* ctx = (Ctx*)c;
    ctx->hit++;
    FuriString* s = furi_string_alloc();
    volvo_rke_protocol.decoder->get_string(ctx->dec, s);
    snprintf(ctx->last, sizeof(ctx->last), "%s", furi_string_get_cstr(s));
    furi_string_free(s);
}

static int run_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if(!f) {
        printf("  CANNOT OPEN %s\n", path);
        return 0;
    }
    static char env;
    void* dec = volvo_rke_protocol.decoder->alloc((SubGhzEnvironment*)&env);
    SubGhzProtocolDecoderBase* base = (SubGhzProtocolDecoderBase*)dec;
    Ctx ctx = {0};
    ctx.dec = dec;
    subghz_protocol_decoder_base_set_decoder_callback(base, rx_cb, &ctx);

    char* line = NULL;
    size_t cap = 0;
    // getline is POSIX; fall back to fixed buffer if unavailable.
    char buf[1 << 16];
    while(fgets(buf, sizeof(buf), f)) {
        if(strncmp(buf, "RAW_Data:", 9) != 0) continue;
        char* p = buf + 9;
        for(;;) {
            while(*p == ' ' || *p == '\t') p++;
            if(*p == '\0' || *p == '\n' || *p == '\r') break;
            char* end;
            long v = strtol(p, &end, 10);
            if(end == p) break;
            p = end;
            bool level = v > 0;
            uint32_t dur = (uint32_t)(v < 0 ? -v : v);
            if(dur == 0) continue;
            volvo_rke_protocol.decoder->feed(dec, level, dur);
        }
    }
    (void)line;
    (void)cap;
    fclose(f);
    printf("  hits=%d\n", ctx.hit);
    if(ctx.hit) printf("  last decode:\n%s\n", ctx.last);
    volvo_rke_protocol.decoder->free(dec);
    return ctx.hit;
}

int main(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: %s <file.sub> [more.sub ...]\n", argv[0]);
        return 2;
    }
    int total = 0;
    for(int i = 1; i < argc; i++) {
        printf("==== %s ====\n", argv[i]);
        total += run_file(argv[i]);
    }
    printf("\nTOTAL FALSE/REAL HITS: %d\n", total);
    return 0;
}
