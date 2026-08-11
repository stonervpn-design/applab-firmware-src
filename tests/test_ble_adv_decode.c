#include "../applications/main/ble_spam/ble_adv_decode.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // AD structures: [len][type][data...]
    // Flags(0x01)=0x06 ; 16-bit UUIDs(0x03)=0x180F,0x180A ; TXpower(0x0A)=0xF4(-12) ; MfrData(0xFF)=0x004C.....
    uint8_t adv[] = {
        0x02,0x01,0x06,
        0x05,0x03,0x0F,0x18,0x0A,0x18,
        0x02,0x0A,0xF4,
        0x03,0xFF,0x4C,0x00
    };
    BleAdvInfo info; memset(&info,0,sizeof info);
    ble_adv_decode(adv, sizeof adv, &info);
    assert(info.flags == 0x06);
    assert(info.uuid16_count == 2);
    assert(info.uuid16[0] == 0x180F && info.uuid16[1] == 0x180A);
    assert(info.has_tx_power && info.tx_power == -12);
    assert(info.mfr_present && info.mfr_id == 0x004C);
    // malformed: truncated length must not overread
    uint8_t bad[] = { 0x05,0x03,0x0F };   // claims 5 bytes, only 1 present
    BleAdvInfo i2; memset(&i2,0,sizeof i2);
    ble_adv_decode(bad, sizeof bad, &i2);   // must not crash / must bound-check
    assert(i2.uuid16_count == 0);
    printf("TESTS_PASS\n");
    return 0;
}
