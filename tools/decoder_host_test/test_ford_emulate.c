// Deterministic repro of the device EMULATE + LOAD paths for a REAL .sub.
// Loads the file byte-faithfully into a FlipperFormat, then runs:
//   (1) the protocol ENCODER deserialize  -- the SEND/emulate path (tx_start)
//   (2) the protocol DECODER deserialize   -- the LOAD path (subghz_key_load)
// Prints each status and (via patched host_shim FURI_LOG) the exact branch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <lib/subghz/protocols/base.h>
#include <lib/flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <core/furi_string.h>

extern const SubGhzProtocol ford_protocol_v0;

static FlipperFormat* load_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if(!fp) {
        printf("CANNOT OPEN %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = malloc(n + 1);
    fread(buf, 1, n, fp);
    buf[n] = 0;
    fclose(fp);
    FlipperFormat* ff = flipper_format_string_alloc();
    Stream* s = flipper_format_get_raw_stream(ff);
    stream_write_cstring(s, buf);
    free(buf);
    flipper_format_rewind(ff);
    return ff;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/mnt/c/Users/stone/Downloads/kk.sub";
    static char env;
    const SubGhzProtocol* proto = &ford_protocol_v0;
    printf("PROTO: %s\n", proto->name);

    // ---- (1) ENCODER deserialize (SEND / emulate path) ----
    const SubGhzProtocolEncoder* enc = proto->encoder;
    printf("encoder=%p alloc=%p deserialize=%p yield=%p\n",
        (void*)enc, (void*)(enc ? (void*)enc->alloc : 0),
        (void*)(enc ? (void*)enc->deserialize : 0), (void*)(enc ? (void*)enc->yield : 0));
    if(enc && enc->alloc && enc->deserialize) {
        FlipperFormat* ff = load_file(path);
        if(ff) {
            void* inst = enc->alloc((SubGhzEnvironment*)&env);
            printf("--- encoder deserialize (watch [LOG] lines) ---\n");
            flipper_format_rewind(ff);
            SubGhzProtocolStatus st = enc->deserialize(inst, ff);
            printf("RESULT: encoder deserialize status = %d => %s\n",
                (int)st, st == SubGhzProtocolStatusOk ? "OK (emulate proceeds)"
                                                      : "NON-OK -> Error in protocol parameters");
            if(enc->free) enc->free(inst);
        }
    } else {
        printf("RESULT: NULL/incomplete encoder -> Error in protocol parameters\n");
    }

    // ---- (2) DECODER deserialize (LOAD path) ----
    const SubGhzProtocolDecoder* dec = proto->decoder;
    printf("\n=== DECODER deserialize (load path) ===\n");
    printf("decoder=%p alloc=%p deserialize=%p\n",
        (void*)dec, (void*)(dec ? (void*)dec->alloc : 0),
        (void*)(dec ? (void*)dec->deserialize : 0));
    if(dec && dec->alloc && dec->deserialize) {
        FlipperFormat* ff2 = load_file(path);
        if(ff2) {
            void* dinst = dec->alloc((SubGhzEnvironment*)&env);
            flipper_format_rewind(ff2);
            SubGhzProtocolStatus dst = dec->deserialize(dinst, ff2);
            printf("RESULT: decoder deserialize status = %d => %s\n",
                (int)dst, dst == SubGhzProtocolStatusOk ? "OK (load proceeds)"
                                                        : "NON-OK -> Error in protocol parameters ON LOAD");

            // ---- (3) THE REAL DEVICE EMULATE CHAIN ----
            // device does: decoder.deserialize -> decoder.SERIALIZE (into history)
            // -> encoder.deserialize(that). Reproduce it and dump the middle FF.
            printf("\n=== (3) decode -> RE-SERIALIZE -> encode (the actual device path) ===\n");
            if(dst == SubGhzProtocolStatusOk && dec->serialize && enc && enc->alloc &&
               enc->deserialize) {
                FlipperFormat* mid = flipper_format_string_alloc();
                SubGhzRadioPreset preset = {0};
                preset.name = furi_string_alloc_set("AM650");
                preset.frequency = 433920000;
                SubGhzProtocolStatus sst = dec->serialize(dinst, mid, &preset);
                printf("decoder.serialize (to history FF) status = %d\n", (int)sst);
                // dump the middle FF (what the encoder will actually receive)
                Stream* ms = flipper_format_get_raw_stream(mid);
                stream_rewind(ms);
                char dump[1024];
                size_t rd = stream_read(ms, (uint8_t*)dump, sizeof(dump) - 1);
                dump[rd] = 0;
                printf("---- history FF the encoder receives ----\n%s\n----\n", dump);
                void* einst2 = enc->alloc((SubGhzEnvironment*)&env);
                flipper_format_rewind(mid);
                SubGhzProtocolStatus est = enc->deserialize(einst2, mid);
                printf("RESULT: encoder deserialize (device chain) status = %d => %s\n",
                    (int)est, est == SubGhzProtocolStatusOk ? "OK" : "NON-OK -> code=4 status matches device!");
                if(enc->free) enc->free(einst2);
                furi_string_free(preset.name);
                flipper_format_free(mid);
            }
            if(dec->free) dec->free(dinst);
        }
    }
    return 0;
}
