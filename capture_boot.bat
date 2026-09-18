@echo off
setlocal
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >nul 2>&1
cd /d "%~dp0"
python "%~dp0capture_boot.py" > "%~dp0capture_boot_out.txt" 2>&1
echo CAPTURE_DONE >> "%~dp0capture_boot_out.txt"
endlocal
