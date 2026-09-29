@echo off
setlocal
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "LOG=%~dp0flash_m5ble.log"
> "%LOG%" echo FLASH_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
cd /d "%~dp0"
python -m esptool --chip esp32s3 -p COM10 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size detect --flash_freq 80m 0x0 "build_m5ble\bootloader\bootloader.bin" 0x8000 "build_m5ble\partition-table-8mb.bin" 0x10000 "build_m5ble\furi_esp32.bin" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo FLASH_FAIL) else (>> "%LOG%" echo FLASH_OK)
endlocal
