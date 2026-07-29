#include "../applications/main/can_commander/can_frame.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint32_t id;
    bool ext;

    // --- id parsing ---
    assert(can_frame_parse_id("1FF", false, &id, &ext) && id == 0x1FF && !ext);
    assert(can_frame_parse_id("0x7ff", false, &id, &ext) && id == 0x7FF && !ext);
    assert(can_frame_parse_id("800", false, &id, &ext) && id == 0x800 && ext); // >11bit -> ext
    assert(can_frame_parse_id("18DAF110", false, &id, &ext) && id == 0x18DAF110 && ext);
    assert(can_frame_parse_id("123", true, &id, &ext) && id == 0x123 && ext); // forced ext
    assert(!can_frame_parse_id("", false, &id, &ext)); // empty
    assert(!can_frame_parse_id("XYZ", false, &id, &ext)); // bad hex
    assert(!can_frame_parse_id("20000000", false, &id, &ext)); // > 29-bit max

    // --- data parsing ---
    uint8_t d[8];
    uint8_t n;
    assert(can_frame_parse_data("DE AD BE EF", d, &n) && n == 4 && d[0] == 0xDE && d[3] == 0xEF);
    assert(can_frame_parse_data("deadbeef", d, &n) && n == 4 && d[1] == 0xAD);
    assert(can_frame_parse_data("01,02:03", d, &n) && n == 3 && d[2] == 0x03);
    assert(can_frame_parse_data("", d, &n) && n == 0);
    assert(!can_frame_parse_data("ABC", d, &n)); // odd nibbles
    assert(!can_frame_parse_data("0011223344556677AA", d, &n)); // 9 bytes > 8
    assert(!can_frame_parse_data("ZZ", d, &n)); // bad hex

    // --- build + format ---
    CanFrame f;
    assert(can_frame_build("7DF", "02 01 0C", false, false, &f));
    assert(f.id == 0x7DF && !f.extended && f.dlc == 3 && f.data[0] == 0x02);
    char s[40];
    size_t w = can_frame_format(&f, s, sizeof(s));
    assert(w > 0 && strcmp(s, "7DF#02010C") == 0);

    // extended id keeps 8-wide, RTR shows R
    assert(can_frame_build("18DAF110", "", true, true, &f));
    can_frame_format(&f, s, sizeof(s));
    assert(strcmp(s, "18DAF110#R") == 0);

    // standard id pads to min width 3
    assert(can_frame_build("1", "FF", false, false, &f));
    can_frame_format(&f, s, sizeof(s));
    assert(strcmp(s, "001#FF") == 0);

    // --- preset round-trip ---
    CanFrame a, b;
    assert(can_frame_build("7E0", "22 F1 90", false, false, &a));
    char line[64];
    size_t pn = can_frame_preset_encode("read_vin", &a, line, sizeof(line));
    assert(pn > 0);
    char name[24];
    assert(can_frame_preset_decode(line, name, sizeof(name), &b));
    assert(strcmp(name, "read_vin") == 0);
    assert(b.id == a.id && b.extended == a.extended && b.rtr == a.rtr && b.dlc == a.dlc);
    assert(memcmp(b.data, a.data, a.dlc) == 0);

    // extended+rtr preset round-trips flags
    assert(can_frame_build("1ABCDEF0", "", true, true, &a));
    can_frame_preset_encode("wake", &a, line, sizeof(line));
    assert(can_frame_preset_decode(line, name, sizeof(name), &b));
    assert(strcmp(name, "wake") == 0 && b.extended && b.rtr && b.dlc == 0 && b.id == 0x1ABCDEF0);

    // malformed preset (too few fields) rejected
    assert(!can_frame_preset_decode("only,two", name, sizeof(name), &b));

    printf("test_can_frame OK\n");
    return 0;
}
