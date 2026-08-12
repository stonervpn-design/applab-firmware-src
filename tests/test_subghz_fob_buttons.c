// Host unit test for the generic car-fob button-table provider.
// Single translation unit: include the .c directly (like tests/test_fob_nav.c),
// so no furi/app headers are pulled in. Build+run under WSL:
//   gcc -Wall -I applications/main/subghz/helpers
//       tests/test_subghz_fob_buttons.c -o /tmp/tsfb  (then run /tmp/tsfb)
#include "../applications/main/subghz/helpers/subghz_fob_buttons.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Assert the table for `name` has exactly the given codes, in order.
static void expect_codes(const char* name, const uint8_t* codes, size_t n) {
    size_t got_n = 12345; // poison, must be overwritten
    const SubGhzFobButton* b = subghz_fob_buttons(name, &got_n);
    assert(b != NULL);
    assert(got_n == n);
    for(size_t i = 0; i < n; i++) {
        assert(b[i].code == codes[i]);
        assert(b[i].name != NULL);
        assert(b[i].name[0] != '\0');
    }
    assert(subghz_fob_protocol_supported(name));
}

int main(void) {
    // VAG: 3 tiles Unlock/Lock/Boot, codes 0x1/0x2/0x4 (preserves existing view).
    const uint8_t vag[] = {0x1, 0x2, 0x4};
    expect_codes("VAG", vag, 3);

    // Generic KeeLoq: 4 tiles Lock/Unlock/Trunk/Panic, 0x1/0x2/0x4/0x8.
    const uint8_t keeloq[] = {0x1, 0x2, 0x4, 0x8};
    expect_codes("KeeLoq", keeloq, 4);

    // One Honda: Honda Static real button set (valid buttons per 0x336 mask).
    const uint8_t honda[] = {0x1, 0x2, 0x4, 0x5, 0x8, 0x9};
    expect_codes("Honda Static", honda, 6);

    // One Kia: Kia V0 base map Lock/Unlock/Trunk = 0x1/0x2/0x3.
    const uint8_t kia[] = {0x1, 0x2, 0x3};
    expect_codes("Kia V0", kia, 3);

    // One Ford: Ford V0 map Panic/Lock/Unlock/Boot = 0x1/0x2/0x4/0x8.
    const uint8_t ford[] = {0x1, 0x2, 0x4, 0x8};
    expect_codes("Ford V0", ford, 4);

    // A rolling Honda variant (V7) that reads an independent "Button" field.
    const uint8_t honda_v7[] = {0x0, 0x1, 0x2, 0x4, 0x8, 0x9};
    expect_codes("HondaV7", honda_v7, 6);

    // Unknown protocol -> (NULL, 0).
    size_t n = 999;
    assert(subghz_fob_buttons("NotARealProtocol", &n) == NULL);
    assert(n == 0);
    assert(subghz_fob_buttons("NotARealProtocol", NULL) == NULL);
    assert(!subghz_fob_protocol_supported("NotARealProtocol"));
    assert(!subghz_fob_protocol_supported(NULL));

    // A genuinely replay-only car protocol (Scher-Khan / MAGIC CODE replays the
    // captured bitstream verbatim; Btn is informational, no name map) -> unsupported.
    assert(subghz_fob_buttons("Scher-Khan", &n) == NULL);
    assert(n == 0);
    assert(!subghz_fob_protocol_supported("Scher-Khan"));

    printf("test_subghz_fob_buttons OK\n");
    return 0;
}
