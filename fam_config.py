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
    "tagtinker",
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
    "camera_hunter",
    "doorbell_brute",
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
    "esubghz_chat",
    "grove_i2c_scan",
    "st25r_nfc_diag",
    # Flipper Share NFC (ISO14443-3A device-to-device file transfer over the
    # ST25R3916 backend, same HAL as st25r_nfc_diag/bus_tools above). Phase 4,
    # 2026-07-31 — send+receive engine complete; on-device acceptance pending.
    # NOTE: this appid was missing from APPS entirely until this point, so it
    # had never actually been compiled into any firmware build (Tasks 1-4's
    # "SENTINEL_OK / clean grep" results only proved the file list; the app
    # source itself was never part of the build).
    "nfc_share",
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
]

# Boards without NFC / IR hardware – exclude the corresponding apps
_board = os.environ.get("FLIPPER_BOARD", "")
# Build variant: "private" (default, everything) | "public" (car stuff stripped)
# | "hardened" (full features + RE-friction compile flags via
# sdkconfig.defaults.hardened). ONLY "public" strips the car stuff; hardened keeps
# all features. Set by the build_m5stick_s3[_public|_hardened].bat scripts; also
# mirrored into a PUBLIC_BUILD compile-def in the root CMakeLists.txt (strips the
# Automotive registries for public). Here it drops the vehicle apps themselves
# (see end of file), again for public only.
_variant = os.environ.get("FLIPPER_VARIANT", "private").strip().lower()
_is_public = _variant == "public"
# StickS3 NFC = external M5Stack NFC Unit (ST25R3916 @0x50 on Grove/QWIIC), backend
# verified working — so the core `nfc` app IS kept (matches Cardputer ADV).
# T-Embed CC1101 phase 1 keeps NFC (PN532) off.
_boards_without_nfc = {"lilygo_t_embed_cc1101"}
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

# M5Stack StickS3: no LoRa / GPS. Strip apps needing hardware this board lacks;
# core services + WiFi/BLE + IR + SubGHz + nRF24 + RFID2 + games/tools + JS +
# pocketlab/subhound remain.
if _board == "m5stick_s3":
    _stick_drop = {
        # NFC suite (nfc/mifare_fuzzer/nfc_magic/key_copier) KEPT on the StickS3:
        # its external M5Stack NFC Unit (ST25R3916 @0x50 via Grove) backend is
        # verified working, matching the Cardputer ADV. rfid2_reader (WS1850S)
        # also kept. (2026-07-27 — public build now has the full NFC suite.)
        # LoRa / GPS / mesh (Cap-LoRa-1262 addon)
        "lora_monitor", "meshtastic_sniffer", "wardriver",
        # external-HW / heavy
        "wiegand", "doom", "wolf3d",
    }
    # CC1101 (RF Pack S3 on SPI3) is enabled and HW-verified, so the SubGHz-
    # dependent apps link and run: radio_scanner, can_commander, and defender
    # (its RF-jammer scene needs the subghz library) are all kept. (proto_pirate
    # removed globally — car decoders are in the main SubGhz Automotive registry.)
    APPS = [a for a in APPS if a not in _stick_drop]

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

# Cardputer-ADV RAM trim (2026-08-04, user "trim the ram... nothing subghz"): drop
# clearly-non-subghz clutter apps to free DRAM/flash. ONLY apps listed in
# OPTIONAL_APPS below are removed (that set is the file's own contract for what can be
# dropped without breaking the link — services/settings are never touched). subghz +
# the whole NFC/RF security toolkit (Evil Portal/wlan, camera_hunter, wardriver,
# defender, nrf24, infrared, nfc suite, esubghz_chat, subhound, can_commander,
# radio_scanner, doorbell_brute) are KEPT. To reclaim more RAM, remove `wlan` (~10 KB,
# the biggest non-subghz app) or disable BLE (~27 KB) — both intentionally left in.
if _board == "m5stack_cardputer_adv":
    _cardputer_drop = {
        # games / toys
        "asteroids", "game15", "flipper_pong", "snake20", "tetris", "texas_holdem",
        "blackjack", "dice_app", "flippergotchi",
        # demos / utilities with no radios
        "pocketlab", "qrcode", "clock", "grove_i2c_scan",
        # LoRa / mesh — need the Cap-LoRa-1262 SX1262 addon; dead weight without it
        "lora_monitor", "meshtastic_sniffer",
        # NOTE: ble_spam is intentionally KEPT — it is NOT a leaf app. The defender
        # app (kept) links its HAL (ble_walk_hal_*, ble_tracker_hal_*) in the BLE-spam
        # and BLE-tracker scenes, so dropping ble_spam breaks the link.
    }
    APPS = [a for a in APPS if a not in _cardputer_drop]

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
# The Sub-GHz Automotive car-key decoder registries are compiled out via
# PUBLIC_BUILD (root CMakeLists.txt + lib/subghz/protocols/protocol_items.c) for
# the public variant. Here we also drop the vehicle app itself: CAN Commander
# (OBD/CAN bus). Defender is NOT car-related (it's a general WiFi/BLE/SubGHz/nRF24/
# drone RF attack detector) so it stays in the public build. Private and hardened
# both keep everything.
if _is_public:
    _car_apps = {"can_commander"}
    APPS = [a for a in APPS if a not in _car_apps]
    # TagTinker (IR ESL / e-ink shelf-label tool) is PRIVATE-only per the owner's
    # scope; never ship it in public webflasher builds.
    APPS = [a for a in APPS if a != "tagtinker"]
    # Camera Hunter (Wi-Fi IP-camera discovery + default-credential testing) is
    # PRIVATE-only: an aggressive authorized-audit tool kept out of public bins.
    APPS = [a for a in APPS if a != "camera_hunter"]
    # Doorbell Brute (Sub-GHz doorbell chime brute-forcer) is PRIVATE-only.
    APPS = [a for a in APPS if a != "doorbell_brute"]
    # ProtoPirate is a CAR RKE toolkit (VAG/Kia/Ford/Honda/PSA/etc. decoders AND
    # encoders). It ships live car decode/encode code + car model names, so by
    # default it is NEVER in a public build (the owner's absolute "no car material
    # on public" rule).
    #
    # OWNER-AUTHORIZED EXCEPTION (2026-08-10, password Romandoe420): proto_pirate is
    # offered on the public applab catalog, but built into a public bin ONLY when a
    # request EXPLICITLY selects it (FLIPPER_APPS lists it — the applab custom-build
    # path). It is never baked into the default full public webflasher bins (those
    # are built with no FLIPPER_APPS narrowing, so the append below never fires).
    # Its own bundled decoders are self-contained; the main app's 66 car protocol
    # sources are already CMake-excluded on public, so there is no dup-symbol clash.
    _pp_sel = {a.strip() for a in os.environ.get("FLIPPER_APPS", "").split(",") if a.strip()}
    if "proto_pirate" in _pp_sel:
        if "proto_pirate" not in APPS:
            APPS.append("proto_pirate")
    else:
        APPS = [a for a in APPS if a != "proto_pirate"]

# ── On-demand build service: custom app subset ───────────────────────────────
# FLIPPER_APPS = comma-separated appids to KEEP. Applied AFTER all board/variant
# filtering, so a request can only NARROW the safe set — it can never re-enable an
# app the board can't run or the variant strips (e.g. car apps in a public build).
# Empty/unset keeps the full board+variant set, so normal builds are unaffected.
# Core/system apps are not in APPS, so the firmware always boots regardless.
# Only these user-selectable apps may be dropped by a custom build. Everything else
# in APPS is a service / settings / system app that non-app code links against
# (e.g. Settings -> power_reboot from the `power` service), so it's ALWAYS compiled —
# dropping it breaks the link. This set mirrors the applab catalog.
OPTIONAL_APPS = {
    "subghz", "subghz_remote", "esubghz_chat", "lora_monitor", "meshtastic_sniffer",
    "nfc", "nfc_magic", "mifare_fuzzer", "lfrfid", "wiegand", "infrared",
    "flipper_xremote", "ibutton", "ble_spam", "wlan", "esp_now", "nrf24",
    "fz_nrf24_jammer", "lan_attack", "bad_usb", "u2f", "gpio", "pin_config",
    "grove_i2c_scan", "bus_tools", "power_monitor", "doom", "wolf3d", "asteroids",
    "blackjack", "flipper_pong", "game15", "dice_app", "flippergotchi", "mp3_player",
    "clock", "qrcode", "pocketlab", "defender",
    # Owner-authorized on public via the applab custom-build only (see the public
    # block above); built solely when explicitly selected.
    "proto_pirate",
}
_apps_env = os.environ.get("FLIPPER_APPS", "").strip()
if _apps_env:
    _want = {a.strip() for a in _apps_env.split(",") if a.strip()}
    # Keep every core app; among OPTIONAL apps keep only the requested ones.
    APPS = [a for a in APPS if a not in OPTIONAL_APPS or a in _want]

EXTRA_EXT_APPS = []
TARGET_HW = 32
AUTORUN_APP = ""
