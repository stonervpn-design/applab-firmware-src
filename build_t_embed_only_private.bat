@echo off
setlocal
REM T-Embed CC1101+ PRIVATE build from the superset tree. Mirrors
REM build_adv_only_private.bat: skips idf.py set-target (Python 3.14 component-manager
REM HTTPStatus crash) and seeds the target via -DIDF_TARGET on the CMake configure.
REM Deletes the shared root sdkconfig first so the build re-seeds from the T-Embed
REM defaults (partitions_lilygo_t_embed.csv, 16MB flash, 8MB OPI PSRAM). FLIPPER_VARIANT
REM =private keeps ALL apps (Car Keys / KeeLoq / ProtoPirate); no -DFLIPPER_VARIANT=public
REM so PUBLIC_BUILD stays off. Produces the merged deliverable bin.
set "LOG=%~dp0build_t_embed_only_private.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
set "FLIPPER_VARIANT=private"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.lilygo_t_embed_cc1101"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_t_embed -DIDF_TARGET=esp32s3 -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-lilygo_t_embed_cc1101-merged.bin" --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "build_t_embed\bootloader\bootloader.bin" 0x8000 "build_t_embed\partition_table\partition-table.bin" 0x10000 "build_t_embed\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
