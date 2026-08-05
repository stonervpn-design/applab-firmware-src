@echo off
setlocal
REM Finish the public T-Embed build after the st25r3916 stub fix: build-only
REM (NO set-target — the build_t_embed_public dir is already configured) + merge.
set "LOG=%~dp0build_lilygo_public.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
set "FLIPPER_VARIANT=public"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.lilygo_t_embed_cc1101"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_t_embed_public -DFLIPPER_BOARD=lilygo_t_embed_cc1101 -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-lilygo_t_embed_cc1101-public-merged.bin" --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "build_t_embed_public\bootloader\bootloader.bin" 0x8000 "build_t_embed_public\partition_table\partition-table.bin" 0x10000 "build_t_embed_public\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_VERIFY_NOCAR
python "%~dp0tools\verify_public_no_car.py" "%~dp0Flipper-lilygo_t_embed_cc1101-public-merged.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_CARKEY_LEAK & del /f /q "%~dp0Flipper-lilygo_t_embed_cc1101-public-merged.bin" >> "%LOG%" 2>&1 & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
