@echo off
setlocal
REM Build LilyGO T-Embed CC1101 firmware (standalone) and produce a merged binary.
set "LOG=%~dp0build_lilygo_t_embed.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.lilygo_t_embed_cc1101"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_SETTARGET
idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -DSDKCONFIG_DEFAULTS="%SDKDEF%" set-target esp32s3 >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_SETTARGET_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_t_embed -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -DSDKCONFIG_DEFAULTS="%SDKDEF%" reconfigure build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-lilygo_t_embed_cc1101-merged.bin" --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "build_t_embed\bootloader\bootloader.bin" 0x8000 "build_t_embed\partition_table\partition-table.bin" 0x10000 "build_t_embed\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_OK
:done
endlocal
