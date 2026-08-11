@echo off
setlocal
REM Incremental M5Stick S3 build (no set-target): recompiles changed files in the
REM already-configured build_m5stick_s3 dir, then re-merges. Avoids the
REM set-target-on-existing-dir configure failure.
set "LOG=%~dp0inc_build_m5stick.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_m5stick_s3 build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-m5stick_s3-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "build_m5stick_s3\bootloader\bootloader.bin" 0x8000 "build_m5stick_s3\partition_table\partition-table.bin" 0x10000 "build_m5stick_s3\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_OK
:done
endlocal
