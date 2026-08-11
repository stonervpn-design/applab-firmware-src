@echo off
setlocal
set "LOG=%~dp0flash_m5stick_com10.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
>> "%LOG%" echo SENTINEL_FLASH
python -m esptool --chip esp32s3 -p COM10 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 "Flipper-m5stick_s3-private-merged.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_FLASH_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
