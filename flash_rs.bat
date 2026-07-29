@echo off
REM Flash/probe helper for the reverse_shell rebuild.
REM   flash_rs.bat probe COM12      -> read chip id (no write)
REM   flash_rs.bat write COM12      -> write merged bin at 0x0
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >nul 2>&1
if %errorlevel% neq 0 (echo === EXPORT FAILED === & exit /b 90)
cd /d "%~dp0"

if /I "%~1"=="probe" (
  python -m esptool --chip esp32s3 -p %~2 flash_id
  exit /b %errorlevel%
)

if /I "%~1"=="write" (
  python -m esptool --chip esp32s3 -p %~2 --before default_reset --after hard_reset ^
    write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m ^
    0x0 "Flipper-cardputer_adv-merged.bin"
  exit /b %errorlevel%
)

echo usage: flash_rs.bat [probe^|write] COMxx
exit /b 1
