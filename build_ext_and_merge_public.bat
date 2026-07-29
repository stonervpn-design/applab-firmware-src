@echo off
setlocal
REM Build a WL-FATFS image of the /ext content (_extfs_stage) for the M5Stick S3
REM "storage" partition, then merge it into the PUBLIC firmware so the web flasher
REM ships the SubGHz keystore + NFC dictionaries (fixes "Keystore database not found").
REM
REM Partition (partitions_m5stick_s3.csv): storage, data, fat, off=0x390000, size=0x460000.
REM FATFS config (sdkconfig): SECTOR_4096, LFN heap, codepage 437 -> sector_size 4096
REM + --long_name_support. Backed by wear_levelling (wl_mount) -> wl_fatfsgen.py.
set "LOG=%~dp0build_ext_and_merge_public.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "STAGE=%~dp0_extfs_stage"
set "STORAGE_BIN=%~dp0build_m5stick_s3_public\storage.bin"
set "WLGEN=C:\Espressif\frameworks\esp-idf-v5.4.1\components\fatfs\wl_fatfsgen.py"
set "BUILD=%~dp0build_m5stick_s3_public"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_FATFSGEN
python "%WLGEN%" "%STAGE%" --output_file "%STORAGE_BIN%" --partition_size 0x460000 --sector_size 4096 --long_name_support >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_FATFSGEN_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_MERGE
python -m esptool --chip esp32s3 merge_bin -o "Flipper-m5stick_s3-public-merged.bin" --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "%BUILD%\bootloader\bootloader.bin" 0x8000 "%BUILD%\partition_table\partition-table.bin" 0x10000 "%BUILD%\furi_esp32.bin" 0x390000 "%STORAGE_BIN%" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_MERGE_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_OK
:done
endlocal
