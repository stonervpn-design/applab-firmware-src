@echo off
setlocal
REM Hardened Cardputer-ADV PRIVATE build from the superset tree. Mirrors
REM build_only_private.bat: skips idf.py set-target (Python 3.14 component-manager
REM HTTPStatus crash) and seeds the target via -DIDF_TARGET on the CMake configure.
REM Deletes the shared root sdkconfig first so the build re-seeds from the CARDPUTER
REM defaults (the Cardputer-ADV has NO PSRAM; inheriting the m5stick 8MB-PSRAM config
REM boot-loops the board). Produces the merged deliverable bin.
set "LOG=%~dp0build_adv_only_private.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stack_cardputer_adv"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_cardputer_adv -DIDF_TARGET=esp32s3 -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_cardputer_adv\bootloader\bootloader.bin" 0x8000 "build_cardputer_adv\partition_table\partition-table.bin" 0x10000 "build_cardputer_adv\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
