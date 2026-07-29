@echo off
REM Non-interactive Cardputer ADV build (no pause). Uses the tools path where the
REM ESP-IDF toolchain is actually installed on this machine (default ~/.espressif),
REM not the C:\Espressif that stock build_adv.bat assumes.
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat"
if %errorlevel% neq 0 (echo === EXPORT FAILED === & exit /b 90)
cd /d "%~dp0"

echo === set-target ===
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" set-target esp32s3
if %errorlevel% neq 0 (echo === SET-TARGET FAILED === & exit /b 91)

echo === build ===
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" reconfigure build
if %errorlevel% neq 0 (echo === BUILD FAILED === & exit /b 92)

echo === merge_bin ===
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-merged.bin" ^
  --flash_mode dio --flash_size 8MB --flash_freq 80m ^
  0x0 "build_cardputer_adv\bootloader\bootloader.bin" ^
  0x8000 "build_cardputer_adv\partition_table\partition-table.bin" ^
  0x10000 "build_cardputer_adv\furi_esp32.bin"
if %errorlevel% neq 0 (echo === MERGE FAILED === & exit /b 93)

echo === BUILD OK ===
exit /b 0
