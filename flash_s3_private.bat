@echo off
REM Flash the PRIVATE M5Stick S3 Flipper merged image.
REM   flash_s3_private.bat probe   -> read chip id (no write)
REM   flash_s3_private.bat write   -> write merged bin at 0x0
setlocal
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "PORT=COM10"
set "BIN=Flipper-m5stick_s3-private-merged.bin"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >nul 2>&1
if %errorlevel% neq 0 (echo === EXPORT FAILED === & exit /b 90)
cd /d "%~dp0"

if /I "%~1"=="probe" (
  python -m esptool --chip esp32s3 -p %PORT% flash_id
  exit /b %errorlevel%
)

if /I "%~1"=="write" (
  python -m esptool --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after hard_reset ^
    write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m ^
    0x0 "%BIN%"
  exit /b %errorlevel%
)

echo usage: flash_s3_private.bat [probe^|write]
exit /b 1
