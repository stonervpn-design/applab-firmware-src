@echo off
setlocal
REM Hardened Cardputer-ADV PUBLIC build from the superset tree. Mirrors
REM build_only_public.bat: skips idf.py set-target (Python 3.14 component-manager
REM HTTPStatus crash) and seeds the target via -DIDF_TARGET on the CMake configure.
REM Deletes the shared root sdkconfig first so the build re-seeds from the CARDPUTER
REM defaults (no PSRAM; the m5stick 8MB-PSRAM config boot-loops the board). Public =
REM FLIPPER_VARIANT=public (ProtoPirate car decoders + can_commander stripped;
REM Defender kept). Cardputer-ADV uses a real SD card, so no /ext partition is merged
REM (firmware bin only). Produces the merged deliverable bin.
set "LOG=%~dp0build_adv_only_public.log"
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
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_cardputer_adv_public -DIDF_TARGET=esp32s3 -DFLIPPER_BOARD=m5stack_cardputer_adv -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-public-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_cardputer_adv_public\bootloader\bootloader.bin" 0x8000 "build_cardputer_adv_public\partition_table\partition-table.bin" 0x10000 "build_cardputer_adv_public\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
