cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# ---------------------------------------------------------------------------
# Patch libnet80211.a to allow raw TX of deauth/disassoc management frames.
# The closed-source WiFi blob rejects frame types 0xC0/0xA0; we patch
# ieee80211_raw_frame_sanity_check() to return 0. Idempotent — skips if
# already patched.  Runs at configure time (before library path resolution).
# ---------------------------------------------------------------------------
set(_wifi_lib "$ENV{IDF_PATH}/components/esp_wifi/lib/esp32s3/libnet80211.a")

# Locate the xtensa toolchain (ar/objcopy) + a python interpreter. Note:
#  - find_program does NOT expand a literal "*" in PATHS, so glob the versioned
#    install dir with file(GLOB) first and pass the results as HINTS.
#  - Do NOT use NO_DEFAULT_PATH: the toolchain bin is on PATH during an IDF
#    build, so PATH is the reliable fallback. The install roots differ by OS
#    (Windows offline installer: IDF_TOOLS_PATH=C:\Espressif; Linux/mac:
#    ~/.espressif) — probe both.
file(GLOB _xt_bindirs
    "$ENV{IDF_TOOLS_PATH}/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin"
    "$ENV{HOME}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin")
find_program(_xt_ar NAMES xtensa-esp32s3-elf-ar HINTS ${_xt_bindirs})
find_program(_xt_objcopy NAMES xtensa-esp32s3-elf-objcopy HINTS ${_xt_bindirs})
find_program(_py NAMES python python3)

if(_xt_ar AND _xt_objcopy AND _py AND EXISTS "${_wifi_lib}")
    execute_process(
        COMMAND "${_py}" "${CMAKE_SOURCE_DIR}/tools/patch_wifi_lib.py"
            "${_wifi_lib}" "${_xt_objcopy}" "${_xt_ar}" "${_wifi_lib}"
        RESULT_VARIABLE _patch_rc OUTPUT_VARIABLE _patch_out ERROR_VARIABLE _patch_err)
    if(_patch_rc EQUAL 0)
        message(STATUS "deauth WiFi-lib patch: ${_patch_out}")
    else()
        message(WARNING "deauth WiFi-lib patch FAILED (rc=${_patch_rc}): ${_patch_err}")
    endif()
else()
    # Loud, not silent: an unpatched blob makes every deauth/disassoc TX fail.
    message(WARNING "deauth WiFi-lib patch SKIPPED — deauth/disassoc frames will be "
        "REJECTED by the closed-source WiFi blob. ar='${_xt_ar}' objcopy='${_xt_objcopy}' "
        "python='${_py}' lib='${_wifi_lib}'")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---------------------------------------------------------------------------
# Build variant: private (default, everything) | public (car stuff stripped).
# ONLY the public variant defines PUBLIC_BUILD, which compiles out the Sub-GHz
# Automotive (car-key) decoder registry in lib/subghz/protocols/protocol_items.c.
# Used by the applab public build service (FLIPPER_VARIANT=public); fam_config.py
# reads the same var to drop the car apps (can_commander) for public only.
# Set HERE — after project.cmake is included (so idf_build_set_property is defined)
# and before project() (so it lands in every component's compile).
# ---------------------------------------------------------------------------
if(NOT DEFINED FLIPPER_VARIANT AND DEFINED ENV{FLIPPER_VARIANT})
    set(FLIPPER_VARIANT "$ENV{FLIPPER_VARIANT}")
endif()
if(NOT DEFINED FLIPPER_VARIANT)
    set(FLIPPER_VARIANT "private")
endif()
if(FLIPPER_VARIANT STREQUAL "public")
    idf_build_set_property(COMPILE_DEFINITIONS "PUBLIC_BUILD=1" APPEND)
    message(STATUS "Flipper variant: public (PUBLIC_BUILD -> car stuff stripped)")
else()
    message(STATUS "Flipper variant: ${FLIPPER_VARIANT} (full features)")
endif()

project(furi_esp32)
