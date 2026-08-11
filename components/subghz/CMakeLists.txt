file(GLOB_RECURSE SUBGHZ_SOURCES
    "../../lib/subghz/*.c"
    "../../lib/drivers/cc1101.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.c"
)

# --- PUBLIC build guardrail: strip car RKE decoders (no car material on public) ---
# These 66 automotive protocol + cipher sources must NEVER compile into a public
# build. protocol_items.c already #ifndef PUBLIC_BUILD's their REGISTRATION, and the
# app-side car helpers are guarded, so nothing references them in a public build;
# removing them from the source list also removes their name strings from the bin.
# The list is the protocol_items.c automotive registry + its ciphers, derived by
# tools (see _preset_backups/car_protocol_files.txt). Any car symbol still referenced
# by unguarded code becomes a link error in the public build — that is the intended
# tripwire, not a bug.
set(_flipper_variant "$ENV{FLIPPER_VARIANT}")
if(DEFINED FLIPPER_VARIANT)
    set(_flipper_variant "${FLIPPER_VARIANT}")
endif()
if(_flipper_variant STREQUAL "public")
    list(FILTER SUBGHZ_SOURCES EXCLUDE REGEX
        "/protocols/(audi_v1|audi_v2|audi_v3|aut64|bmw_cas4|bmw_v2|bmw_v3|chevrolet_v0|chrysler_v0|ducato_v0|fiat_spa|fiat_v0|fiat_v1|fiat_v2_v3_v5|ford_v0|ford_v1|ford_v2|ford_v3|hitag2|hitag2_cipher|honda_static|honda_v10|honda_v2_v3|honda_v4|honda_v5|honda_v6|honda_v7|honda_v8|honda_v9|hyundai_kia_rio|hyundai_tl|jeep_v0|kia_v0|kia_v1|kia_v2|kia_v3_v4|kia_v5|kia_v6|kia_v7|land_rover_v0|ldv_v0|lexus_v0|mazda_siemens|mazda_v0|mazda_v1|mazda_v2|mitsubishi_v0|mitsubishi_v1|nissan_v0|opel_v0|peugot_v0|porsche_touareg|psa|psa_bf_core|psa_crypto|psa_crypto_bf|renault_v1|renault_v2|renault_v3|renault_v4|scher_khan|sheriff_cfm|star_line|subaru|toyota_v0|vag)\\.c$")
    message(STATUS "subghz: PUBLIC build -> car RKE protocol/cipher sources excluded")
endif()

idf_component_register(
    SRCS ${SUBGHZ_SOURCES}
    INCLUDE_DIRS
        ".."
        "../../lib/drivers"
        "../../lib/subghz"
        # NICHT "../../lib/subghz/blocks": dessen math.h enthaelt nur subghz-CRC-Helfer
        # (keine libm-Prototypen) und wuerde, global exportiert, jedes <math.h> ueberschatten
        # -> implizite Deklarationen von log/sqrt/sinf/floorf/... in furi_hal/js_app/nfc.
        # subghz-intern wird der Header relativ inkludiert ("math.h" / "../blocks/math.h"),
        # externe Nutzer ueber <lib/subghz/blocks/...> bzw. firmware_api ueber "blocks/math.h"
        # (relativ zu lib/subghz) -- keiner braucht diesen Pfad als -I.
        "../../lib/subghz/devices"
        "../../lib/subghz/protocols"
    REQUIRES furi furi_hal toolbox mlib flipper_application storage flipper_format rpc
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
)
