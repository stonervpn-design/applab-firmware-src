#include "loader_menu_categories.h"
#include <string.h>

typedef struct {
    const char* appid;
    LoaderCategoryId cat;
} LoaderCategoryMapEntry;

// App id -> category. Ids are matched against internal `.appid` and external
// `.path` (both hold the app id string in this port). Entries for apps that are
// not compiled in are harmless: they simply never match.
static const LoaderCategoryMapEntry loader_category_map[] = {
    // Sub-GHz
    {"subghz", LoaderCategorySubGhz},
    {"subghz_remote_ofw", LoaderCategorySubGhz},
    {"radio_scanner", LoaderCategorySubGhz},
    {"proto_pirate", LoaderCategorySubGhz},
    {"weather_station", LoaderCategorySubGhz},
    // 2.4 GHz
    {"nrf24", LoaderCategory24Ghz},
    {"fz_nrf24_jammer", LoaderCategory24Ghz},
    {"esp_now", LoaderCategory24Ghz},
    // LoRa / Mesh
    {"lora_monitor", LoaderCategoryLora},
    {"meshtastic_sniffer", LoaderCategoryLora},
    // NFC (13.56 MHz)
    {"nfc", LoaderCategoryNfc},
    {"nfc_magic", LoaderCategoryNfc},
    {"mifare_fuzzer", LoaderCategoryNfc},
    {"rfid2_reader", LoaderCategoryNfc},
    // RFID (125 kHz) / iButton
    {"lfrfid", LoaderCategoryRfid},
    {"ibutton", LoaderCategoryRfid},
    // WiFi
    {"wlan", LoaderCategoryWifi},
    {"defender", LoaderCategoryWifi},
    {"lan_attack", LoaderCategoryWifi},
    {"reverse_shell", LoaderCategoryWifi},
    {"wardriver", LoaderCategoryWifi},
    {"flippergotchi", LoaderCategoryWifi},
    // Bluetooth
    {"ble_spam", LoaderCategoryBluetooth},
    {"findmy", LoaderCategoryBluetooth},
    // Infrared
    {"infrared", LoaderCategoryInfrared},
    {"flipper_xremote", LoaderCategoryInfrared},
    // GPIO / Hardware
    {"gpio", LoaderCategoryGpio},
    {"wiegand", LoaderCategoryGpio},
    {"bus_tools", LoaderCategoryGpio},
    {"can_commander", LoaderCategoryGpio},
    {"pin_config", LoaderCategoryGpio},
    // USB / HID
    {"bad_usb", LoaderCategoryUsb},
    {"u2f", LoaderCategoryUsb},
    // Games
    {"tetris", LoaderCategoryGames},
    {"snake20", LoaderCategoryGames},
    {"t_embed_snake", LoaderCategoryGames},
    {"asteroids", LoaderCategoryGames},
    {"game15", LoaderCategoryGames},
    {"texas_holdem", LoaderCategoryGames},
    {"flipper_pong", LoaderCategoryGames},
    {"blackjack", LoaderCategoryGames},
    {"blackjack_split", LoaderCategoryGames},
    {"doom", LoaderCategoryGames},
    {"wolf3d", LoaderCategoryGames},
    {"dice_app", LoaderCategoryGames},
    {"tamagotchi_p1", LoaderCategoryGames},
    // Tools
    {"clock", LoaderCategoryTools},
    {"qrcode", LoaderCategoryTools},
    {"mp3_player", LoaderCategoryTools},
    {"key_copier", LoaderCategoryTools},
    {"passport", LoaderCategoryTools},
    {"power_monitor", LoaderCategoryTools},
};

LoaderCategoryId loader_menu_category_for(const char* appid) {
    if(!appid) return LoaderCategoryMore;
    const size_t n = sizeof(loader_category_map) / sizeof(loader_category_map[0]);
    for(size_t i = 0; i < n; i++) {
        if(strcmp(appid, loader_category_map[i].appid) == 0) {
            return loader_category_map[i].cat;
        }
    }
    return LoaderCategoryMore;
}

static const char* const loader_category_names[LoaderCategoryCount] = {
    [LoaderCategorySubGhz] = "Sub-GHz",
    [LoaderCategory24Ghz] = "2.4 GHz",
    [LoaderCategoryLora] = "LoRa / Mesh",
    [LoaderCategoryNfc] = "NFC",
    [LoaderCategoryRfid] = "RFID / iButton",
    [LoaderCategoryWifi] = "WiFi",
    [LoaderCategoryBluetooth] = "Bluetooth",
    [LoaderCategoryInfrared] = "Infrared",
    [LoaderCategoryGpio] = "GPIO / Hardware",
    [LoaderCategoryUsb] = "USB / HID",
    [LoaderCategoryGames] = "Games",
    [LoaderCategoryTools] = "Tools",
    [LoaderCategoryMore] = "More",
};

const char* loader_menu_category_name(LoaderCategoryId cat) {
    if(cat < 0 || cat >= LoaderCategoryCount) return "?";
    return loader_category_names[cat];
}
