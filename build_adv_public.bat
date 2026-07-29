@echo off
setlocal
REM Build Cardputer ADV firmware, PUBLIC variant, and produce a merged binary.
REM Public = ProtoPirate car-key decoders compiled out (PUBLIC_BUILD in root
REM CMakeLists.txt + lib/subghz/protocols/protocol_items.c) and the vehicle app
REM CAN Commander (can_commander) dropped by fam_config.py; the community
REM standalone ProtoPirate app is added instead. Defender is NOT car-related
REM (WiFi/BLE/SubGHz/nRF24/drone RF attack detector) so it stays.
REM Builds into a SEPARATE dir (build_cardputer_adv_public) so the private
REM build_cardputer_adv tree/binary stays intact. Cardputer-ADV uses a real SD
REM card, so no /ext storage partition is merged (firmware bin only).
set "LOG=%~dp0build_cardputer_adv_public.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stack_cardputer_adv"
set "FLIPPER_VARIANT=public"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_SETTARGET
idf.py -B build_cardputer_adv_public -DFLIPPER_BOARD=m5stack_cardputer_adv -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" set-target esp32s3 >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_SETTARGET_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_cardputer_adv_public -DFLIPPER_BOARD=m5stack_cardputer_adv -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" reconfigure build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-public-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_cardputer_adv_public\bootloader\bootloader.bin" 0x8000 "build_cardputer_adv_public\partition_table\partition-table.bin" 0x10000 "build_cardputer_adv_public\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_OK
:done
endlocal
