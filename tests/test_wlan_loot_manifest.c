#include "../applications/main/wlan_app/wlan_loot_manifest.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    WlanManifestRow r;

    assert(wlan_manifest_parse_line("2026-07-11 06:49:00,pmkid,HomeNet,aabbccdd", &r));
    assert(strcmp(r.timestamp, "2026-07-11 06:49:00") == 0);
    assert(strcmp(r.type, "pmkid") == 0);
    assert(strcmp(r.target, "HomeNet") == 0);
    assert(strcmp(r.detail, "aabbccdd") == 0);

    // empty target + detail
    assert(wlan_manifest_parse_line("2026-07-11 06:50:00,karma,,", &r));
    assert(strcmp(r.type, "karma") == 0);
    assert(r.target[0] == '\0');
    assert(r.detail[0] == '\0');

    // header rejected
    assert(!wlan_manifest_parse_line("timestamp,type,target,detail", &r));
    // blank / trailing newline handling
    assert(!wlan_manifest_parse_line("", &r));
    assert(!wlan_manifest_parse_line("\n", &r));
    assert(wlan_manifest_parse_line("2026,alert,X,deauth flood\n", &r));
    assert(strcmp(r.detail, "deauth flood") == 0); // trailing \n stripped

    // too few fields → false
    assert(!wlan_manifest_parse_line("2026,onlytwo", &r));

    printf("test_wlan_loot_manifest OK\n");
    return 0;
}
