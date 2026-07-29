@echo off
setlocal
set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >nul 2>&1
cd /d "%~dp0"
python "C:\Espressif\frameworks\esp-idf-v5.4.1\components\espcoredump\espcoredump.py" --port COM12 --chip esp32s3 info_corefile "build_cardputer_adv\furi_esp32.elf"
endlocal
