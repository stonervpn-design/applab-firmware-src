file(GLOB_RECURSE SUBGHZ_SOURCES
    "../../lib/subghz/*.c"
    "../../lib/drivers/cc1101.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext.c"
    "../../applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.c"
)

# --- PUBLIC build guardrail: strip the main-app car RKE decoders on public ---
# The private in-tree SubGhz Automotive decoders are already UNREGISTERED on public
# (protocol_items.c #ifndef PUBLIC_BUILD), so they're unreachable there. This drops
# their code + car model-name strings from the binary too, so that when ProtoPirate
# (upstream github.com/RocketGod-git/ProtoPirate) is selected on the applab public
# build, ITS OWN decoders are the only car code present. It also removes the two
# names main-app + ProtoPirate share (star_line, scher_khan), avoiding a dup-symbol
# clash. Verified link-safe: no kept (non-car) source references any of these 34
# registered car decoders (only the guarded registry + the car sources themselves).
set(_flipper_variant "$ENV{FLIPPER_VARIANT}")
if(DEFINED FLIPPER_VARIANT)
    set(_flipper_variant "${FLIPPER_VARIANT}")
endif()
if(_flipper_variant STREQUAL "public")
    list(FILTER SUBGHZ_SOURCES EXCLUDE REGEX
        "/protocols/(audi_v1|audi_v2|audi_v3|bmw_v2|bmw_v3|chevrolet_v0|ducato_v0|fiat_spa|fiat_v2_v3_v5|hitag2|honda_v10|honda_v2_v3|honda_v4|honda_v5|honda_v6|honda_v7|honda_v8|honda_v9|jeep_v0|ldv_v0|lexus_v0|mazda_v1|mazda_v2|mitsubishi_v1|nissan_v0|opel_v0|peugot_v0|renault_v1|renault_v2|renault_v3|renault_v4|scher_khan|star_line|toyota_v0)\\.c$")
    message(STATUS "subghz: PUBLIC build -> main-app car RKE decoders excluded (ProtoPirate-only car code)")
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
