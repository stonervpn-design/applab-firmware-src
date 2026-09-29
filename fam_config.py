import os

MANIFEST_ROOTS = [
    "components",
    "applications",
    "applications_user",
    "port_apps",
]

APP_SOURCE_OVERRIDES = {
    "desktop": "applications",
    "storage": "applications",
}

APPS = [
    "input",
    "notification",
    "gui",
    "dialogs",
    "locale",
    "cli",
    "cli_vcp",
    "storage",
    "storage_start",
    "power",
    "power_start",
    "power_settings",
    "loader",
    "loader_start",
    "notification_settings",
    "desktop",
    "archive",
    "about",
    "bt_settings",
    "example_apps_data",
    "example_apps_assets",
    "example_number_input",
    "clock",
    "qrcode",
    "power_monitor",
    "bad_usb",
    "subghz",
    "cli_subghz",
    "subghz_load_dangerous_settings",
    "passport",
    "nfc",
    "infrared",
    "lfrfid",
    "wlan",
    "nrf24",
    "ble_spam",
    "wiegand",
    "bus_tools",
    "can_commander",
    "pin_config",
    "device_name",
    "defender",
    "lan_attack",
    "js_app",
    "js_event_loop",
    "js_gui",
    "js_gui__loading",
    "js_gui__empty_screen",
    "js_gui__submenu",
    "js_gui__text_input",
    "js_gui__number_input",
    "js_gui__button_panel",
    "js_gui__popup",
    "js_gui__button_menu",
    "js_gui__menu",
    "js_gui__vi_list",
    "js_gui__byte_input",
    "js_gui__text_box",
    "js_gui__dialog",
    "js_gui__file_picker",
    "js_gui__widget",
    "js_gui__icon",
    "js_notification",
    "js_math",
    "js_storage",
    "js_subghz",
    "js_infrared",
    "js_blebeacon",
    # js_serial, js_gpio, js_i2c, js_spi excluded - need HAL porting
    "key_copier",
    # red-team FAPs (bundled in applications_user/)
    # "fz_nrf24_jammer",  # removed per user request
    # "proto_pirate",  # removed — car decoders live in the main SubGhz app now
    #                    (Radio Settings -> Automotive toggle; registry in
    #                    lib/subghz/protocols/protocol_items.c)
    "findmy",
    # red-team FAPs (external, from xMasterX/all-the-plugins)
    "radio_scanner",
    "mifare_fuzzer",
    "nfc_magic",
    # Cap LoRa-1262 addon (SX1262 LoRa + ATGM336H GNSS)
    "wardriver",
    # WiFi STA + TCP dial-out reverse shell (authorized testing only)
    "reverse_shell",
    "lora_monitor",
    "meshtastic_sniffer",
    # M5Stack Unit RFID2 (WS1850S 13.56MHz over I2C)
    "rfid2_reader",
    # Pwnagotchi-style WiFi handshake harvester
    "flippergotchi",
    # Kit-out: already-ported games that shipped disabled. Only the collision-free
    # ones are enabled — blackjack/dice/tamagotchi/t_embed_snake/flipper_xremote
    # duplicate globals/icons of other apps when compiled into one image (they are
    # designed as standalone .faps) and are left off.
    "asteroids",
    "game15",
    "flipper_pong",
    "snake20",
    "tetris",
    "texas_holdem",
    # Tier A batch: previously-disabled ports re-enabled after verifying their
    # fap_icon_assets symbols don't collide with the enabled set (checked against
    # global assets_icons.h + all enabled asset dirs). appid != dir for dice.
    "blackjack",
    "dice_app",
    # "tamagotchi_p1",  # removed per user request
    # Education app (github.com/PerfectoWeb/flipper-pocketlab): pure UI/scene app,
    # no radios/BLE; content is const->flash. EXTERNAL->MENUEXTERNAL in its .fam.
    "pocketlab",
    # Offline SubGhz BinRAW classifier (github.com/maxwalks/subhound): reads
    # /ext/subghz/*.sub, 17-stage heuristic classifier; no live radio. src in flipper-app/.
    "subhound",
    # De Bruijn fixed-code brute forcer (OpenSesame): one continuous OOK sweep covers
    # all 2^N codes (Princeton/CAME/NICE/Holtek). applications_user/subghz_debruijn/.
    "subghz_debruijn",
    # RollBack / Rolling-PWN: capture a consecutive rolling-code sequence -> replay it
    # to resync the receiver's counter. applications_user/subghz_rollback/.
    "subghz_rollback",
]

# Boards without NFC / IR hardware – exclude the corresponding apps
_board = os.environ.get("FLIPPER_BOARD", "")
# Build variant: "private" (default, everything) | "public" (car stuff stripped for
# the applab public build service). Only "public" strips: it drops the car apps below
# and the root CMakeLists.txt mirrors it into a PUBLIC_BUILD compile-def that compiles
# out the Sub-GHz Automotive car-key decoder registry (lib/subghz/protocols/
# protocol_items.c). Unset/"private" => unchanged, so normal momentum builds are the
# same as before.
_variant = os.environ.get("FLIPPER_VARIANT", "private").strip().lower()
_is_public = _variant == "public"
# M5Stack StickS3 has no onboard NFC. T-Embed CC1101 phase 1 keeps NFC (PN532) off.
_boards_without_nfc = {"m5stick_s3", "lilygo_t_embed_cc1101"}
_boards_without_ir = set()
# Neither Cardputer has an RDM6300/RFID front-end (BOARD_HAS_RFID=0); StickS3 has none
# either; the T-Embed CC1101 has no 125 kHz front-end.
_boards_without_lfrfid = {
    "m5stack_cardputer", "m5stack_cardputer_adv", "m5stick_s3", "lilygo_t_embed_cc1101"}

# Wolf3D shares Doom's requirements (PSRAM, ST7789 320xN, I2S speaker).
# Both Cardputer boards excluded (no PSRAM); StickS3 excluded (135x240 panel, Phase-0
# speaker off).
_boards_without_wolf3d = {
    "m5stack_cardputer", "m5stack_cardputer_adv", "m5stick_s3", "lilygo_t_embed_cc1101"}

if _board in _boards_without_nfc:
    APPS = [a for a in APPS if a != "nfc"]

if _board in _boards_without_lfrfid:
    APPS = [a for a in APPS if a != "lfrfid"]

# m5stack_cardputer (standard) has no CC1101 (BOARD_HAS_SUBGHZ=0) → excluded.
# m5stack_cardputer_adv ships SubGHz (BOARD_HAS_SUBGHZ=1) → NOT excluded.
# StickS3 gets its CC1101 from the pingequa RF Pack S3 on SPI3
# (BOARD_HAS_SUBGHZ=1) → NOT excluded.
_boards_without_subghz = {"m5stack_cardputer"}

# NRF24 plugs into the slot. Standard Cardputer has none
# (BOARD_HAS_NRF24=0); Cardputer-ADV does (BOARD_HAS_NRF24=1) → NOT excluded.
# StickS3's RF Pack S3 carries an nRF24 (CS=8/CE=1, CS-muxed on SPI3 alongside
# the CC1101; BOARD_HAS_NRF24=1) → NOT excluded.
_boards_without_nrf24 = {"m5stack_cardputer"}

if _board in _boards_without_ir:
    APPS = [a for a in APPS if a not in ("infrared", "js_infrared")]

if _board in _boards_without_subghz:
    APPS = [a for a in APPS if a not in ("subghz", "cli_subghz", "subghz_load_dangerous_settings", "js_subghz")]

if _board in _boards_without_nrf24:
    APPS = [a for a in APPS if a not in ("nrf24",)]

# ---------------------------------------------------------------------------
# M5Stack StickS3 — STRIPPED build (user request 2026-09-03): keep ONLY the
# user's SubGHz stuff, the nRF24 tool (better jammer, drives the RF-Pack "2in1"),
# and the WiFi attack suite. Everything else (NFC/IR/BLE-spam/LoRa/RFID/games/
# JS/tools) is dropped. Whitelist (not drop-list) so the intent is explicit and
# the set can't silently grow. Board-gated → the Cardputer build is untouched.
# ---------------------------------------------------------------------------
if _board == "m5stick_s3":
    _stick_keep = {
        # --- core services / OS: required to boot + link (started by the loader
        #     or referenced by desktop/settings). Not user-facing "features". ---
        "input", "notification", "gui", "dialogs", "locale", "cli", "cli_vcp",
        "storage", "storage_start", "power", "power_start", "power_settings",
        "loader", "loader_start", "notification_settings", "desktop", "archive",
        "about", "bt_settings", "passport",
        # --- SubGHz: the main app carries the user's automotive decoder registry
        #     (lib/subghz/protocols) + all Sub-GHz protocols. ---
        "subghz", "cli_subghz", "subghz_load_dangerous_settings",
        # De Bruijn fixed-code brute forcer (OpenSesame-style OOK sweep).
        "subghz_debruijn",
        # RollBack / Rolling-PWN sequence capture + replay.
        "subghz_rollback",
        # --- WiFi attacks ---
        "wlan",
        # --- nRF24: jammer + MouseJack + spectrum, on the RF-Pack S3 (SPI3). ---
        "nrf24",
        # --- BadUSB + BadBLE: one app, two transports. USB HID rides the S3's
        #     native USB (USB-C is on the S3 D+/D-, console is USB-Serial-JTAG,
        #     so it can switch to a TinyUSB HID device like the Cardputer does);
        #     BLE HID rides NimBLE (peripheral role already on for this board).
        #     Pulls in ble_hid/ble_profile/assets as fap_libs. ---
        "bad_usb",
    }
    APPS = [a for a in APPS if a in _stick_keep]

# LilyGO T-Embed CC1101 (phase 1): real CC1101 + nRF24 + IR + SD + WiFi/BLE + games/
# tools/JS all kept. Dropped: NFC/RFID apps (PN532 deferred to phase 2, no 125 kHz or
# WS1850S front-end) and LoRa/GPS apps (no Cap LoRa-1262 SX1262 addon on this board).
if _board == "lilygo_t_embed_cc1101":
    _tembed_drop = {
        "mifare_fuzzer", "nfc_magic", "key_copier", "rfid2_reader",
        "lora_monitor", "meshtastic_sniffer", "wardriver",
        "wiegand",
    }
    APPS = [a for a in APPS if a not in _tembed_drop]

# NOTE: cli_vcp and dolphin look like easy RAM wins (~4KB stack each) but both
# are hard-wired into the desktop service: desktop.c calls cli_vcp_enable/disable
# directly (USB-Storage/qFlipper toggles) and lists "dolphin" in its `requires`
# while opening RECORD_DOLPHIN. Dropping either breaks the link / hangs the UI,
# so they stay. The real RAM wins are BLE-off (~68KB) + smaller WiFi buffers.

# qFlipper, USB-Storage and "Switch to Bruce" live in the desktop lock menu
# (applications/services/desktop) which gates them itself.
# Bruce is gated behind a runtime ota_1-partition check.

if _board in _boards_without_wolf3d:
    APPS = [a for a in APPS if a != "wolf3d"]

# --- Build-variant filtering: ONLY public strips the "car stuff" ---
# The Sub-GHz Automotive car-key decoder registry is compiled out via PUBLIC_BUILD
# (root CMakeLists.txt + lib/subghz/protocols/protocol_items.c) for the public variant.
# Here we also drop the vehicle app itself: CAN Commander (OBD/CAN bus). Defender is
# NOT car-related (general WiFi/BLE/SubGHz/nRF24/drone RF detector) so it stays. The
# extra names are private-only tools kept out of public bins defensively even if they
# aren't in this tree today. Private/hardened keep everything.
if _is_public:
    _car_apps = {"can_commander", "tagtinker", "camera_hunter", "doorbell_brute"}
    APPS = [a for a in APPS if a not in _car_apps]

# -- On-demand build service (applab): custom app subset ----------------------------
# FLIPPER_APPS = comma-separated appids to KEEP. Applied AFTER all board/variant
# filtering, so a request can only NARROW the safe set -- it can never re-enable an app
# the board can't run or the variant strips. Empty/unset keeps the full board+variant
# set, so normal builds are unaffected. Only these user-selectable apps may be dropped;
# everything else in APPS is a service/settings/system app that other code links
# against, so it is ALWAYS compiled. Mirrors the shared applab catalog.
OPTIONAL_APPS = {
    "subghz", "subghz_remote", "esubghz_chat", "lora_monitor", "meshtastic_sniffer",
    "nfc", "nfc_magic", "mifare_fuzzer", "lfrfid", "wiegand", "infrared",
    "flipper_xremote", "ibutton", "ble_spam", "findmy", "wlan", "esp_now", "nrf24",
    "fz_nrf24_jammer", "lan_attack", "bad_usb", "u2f", "gpio", "pin_config",
    "grove_i2c_scan", "bus_tools", "power_monitor", "doom", "wolf3d", "asteroids",
    "blackjack", "flipper_pong", "game15", "dice_app", "flippergotchi", "mp3_player",
    "clock", "qrcode", "pocketlab", "defender",
}
# Hidden inter-app link deps: a selected app whose C sources call functions DEFINED in
# another app's translation unit needs that provider co-included or the link fails
# ("undefined reference"). Pull providers in before narrowing. An absent provider is a
# harmless no-op (adding an absent appid to the wanted-set changes nothing).
APP_REQUIRES = {
    "defender": {"ble_spam", "subghz", "wlan"},
    "flippergotchi": {"wlan"},
}
_apps_env = os.environ.get("FLIPPER_APPS", "").strip()
if _apps_env:
    _want = {a.strip() for a in _apps_env.split(",") if a.strip()}
    _stack = list(_want)
    while _stack:
        _dep = _stack.pop()
        for _prov in APP_REQUIRES.get(_dep, ()):
            if _prov not in _want:
                _want.add(_prov)
                _stack.append(_prov)
    APPS = [a for a in APPS if a not in OPTIONAL_APPS or a in _want]

EXTRA_EXT_APPS = []
TARGET_HW = 32
AUTORUN_APP = ""
