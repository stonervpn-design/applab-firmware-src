#include "../components/loader/loader_menu_categories.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // Representative app id per bucket maps to the right category.
    assert(loader_menu_category_for("subghz") == LoaderCategorySubGhz);
    assert(loader_menu_category_for("radio_scanner") == LoaderCategorySubGhz);
    assert(loader_menu_category_for("nrf24") == LoaderCategory24Ghz);
    assert(loader_menu_category_for("fz_nrf24_jammer") == LoaderCategory24Ghz);
    assert(loader_menu_category_for("lora_monitor") == LoaderCategoryLora);
    assert(loader_menu_category_for("meshtastic_sniffer") == LoaderCategoryLora);
    assert(loader_menu_category_for("nfc") == LoaderCategoryNfc);
    assert(loader_menu_category_for("rfid2_reader") == LoaderCategoryNfc); // 13.56 MHz
    assert(loader_menu_category_for("lfrfid") == LoaderCategoryRfid); // 125 kHz
    assert(loader_menu_category_for("ibutton") == LoaderCategoryRfid);
    assert(loader_menu_category_for("wlan") == LoaderCategoryWifi);
    assert(loader_menu_category_for("lan_attack") == LoaderCategoryWifi);
    assert(loader_menu_category_for("ble_spam") == LoaderCategoryBluetooth);
    assert(loader_menu_category_for("findmy") == LoaderCategoryBluetooth);
    assert(loader_menu_category_for("infrared") == LoaderCategoryInfrared);
    assert(loader_menu_category_for("gpio") == LoaderCategoryGpio);
    assert(loader_menu_category_for("bus_tools") == LoaderCategoryGpio);
    assert(loader_menu_category_for("bad_usb") == LoaderCategoryUsb);
    assert(loader_menu_category_for("u2f") == LoaderCategoryUsb);
    assert(loader_menu_category_for("tetris") == LoaderCategoryGames);
    assert(loader_menu_category_for("key_copier") == LoaderCategoryTools);
    assert(loader_menu_category_for("qrcode") == LoaderCategoryTools);

    // Unknown / NULL ids fall into the catch-all so nothing is ever lost.
    assert(loader_menu_category_for("some_new_unmapped_app") == LoaderCategoryMore);
    assert(loader_menu_category_for(NULL) == LoaderCategoryMore);

    // Names are present and sane for every category.
    for(int c = 0; c < LoaderCategoryCount; c++) {
        const char* n = loader_menu_category_name((LoaderCategoryId)c);
        assert(n && n[0] != '\0' && strcmp(n, "?") != 0);
    }
    assert(strcmp(loader_menu_category_name(LoaderCategorySubGhz), "Sub-GHz") == 0);
    assert(strcmp(loader_menu_category_name(LoaderCategoryMore), "More") == 0);

    printf("test_loader_categories OK\n");
    return 0;
}
