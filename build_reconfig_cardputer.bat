@echo off
setlocal
REM Incremental reconfigure build (no set-target) for the PRIVATE Cardputer ADV
REM variant into build_cardputer_adv. Skips idf.py set-target (Python 3.14
REM component-manager crash) — the build dir is already targeted esp32s3.
REM Mirrors build_reconfig_private.bat but for FLIPPER_BOARD=m5stack_cardputer_adv.
set "LOG=%~dp0build_reconfig_cardputer.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stack_cardputer_adv"
set "FLIPPER_VARIANT=private"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
REM Regenerate the shared sdkconfig from the cardputer defaults.
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="%SDKDEF%" reconfigure build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_cardputer_adv\bootloader\bootloader.bin" 0x8000 "build_cardputer_adv\partition_table\partition-table.bin" 0x10000 "build_cardputer_adv\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
