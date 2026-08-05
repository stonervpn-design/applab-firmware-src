@echo off
setlocal
REM Build M5Stack StickS3 firmware, PUBLIC variant, and produce a merged binary.
REM Public = ProtoPirate car-key decoders compiled out (PUBLIC_BUILD in root
REM CMakeLists.txt + lib/subghz/protocols/protocol_items.c) and the vehicle app
REM CAN Commander (can_commander) dropped by fam_config.py. Defender is NOT
REM car-related (WiFi/BLE/SubGHz/nRF24/drone RF attack detector) so it stays.
REM Builds into a SEPARATE dir
REM (build_m5stick_s3_public) so the private build_m5stick_s3 tree/binary stays intact.
set "LOG=%~dp0build_m5stick_s3_public.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
set "FLIPPER_VARIANT=public"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stick_s3"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_SETTARGET
idf.py -B build_m5stick_s3_public -DFLIPPER_BOARD=m5stick_s3 -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" set-target esp32s3 >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_SETTARGET_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_m5stick_s3_public -DFLIPPER_BOARD=m5stick_s3 -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" reconfigure build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-m5stick_s3-public-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_m5stick_s3_public\bootloader\bootloader.bin" 0x8000 "build_m5stick_s3_public\partition_table\partition-table.bin" 0x10000 "build_m5stick_s3_public\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_VERIFY_NOCAR
python "%~dp0tools\verify_public_no_car.py" "%~dp0Flipper-m5stick_s3-public-merged.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_CARKEY_LEAK & del /f /q "%~dp0Flipper-m5stick_s3-public-merged.bin" >> "%LOG%" 2>&1 & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
