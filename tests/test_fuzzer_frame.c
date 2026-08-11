#include "../applications/main/subghz/helpers/fuzzer_frame.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    FuzzFrame f; memset(&f, 0, sizeof f);
    f.nbits = 24; f.bits[0]=0xAA; f.bits[1]=0x55; f.bits[2]=0x0F;
    char buf[512];
    int n = fuzzer_frame_build(&f, FuzzModeProtocol, "Princeton", 400, 5, buf, sizeof buf);
    assert(n > 0);
    assert(strstr(buf, "Protocol: Princeton"));
    assert(strstr(buf, "Bit: 24"));
    assert(strstr(buf, "Key: 00 00 00 00 00 AA 55 0F")); // right-justified 8 bytes
    assert(strstr(buf, "TE: 400"));
    // raw mode
    FuzzFrame r; memset(&r, 0, sizeof r); r.ndur = 4;
    r.dur[0]=400; r.dur[1]=-400; r.dur[2]=400; r.dur[3]=-1200;
    n = fuzzer_frame_build(&r, FuzzModeRaw, NULL, 0, 1, buf, sizeof buf);
    assert(n > 0 && strstr(buf, "Protocol: RAW") && strstr(buf, "RAW_Data: 400 -400 400 -1200"));
    printf("TESTS_PASS\n");
    return 0;
}
