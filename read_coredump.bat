@echo off
setlocal
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.4.1"
set "PYTHONIOENCODING=utf-8"
set "PY=C:\Users\stone\.espressif\python_env\idf5.4_py3.14_env\Scripts\python.exe"
cd /d "%~dp0"
"%PY%" -m esp_coredump --port COM22 --chip esp32s3 info_corefile "build_t_embed\furi_esp32.elf"
endlocal
