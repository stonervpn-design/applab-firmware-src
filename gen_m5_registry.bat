@echo off
set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
set "FLIPPER_VARIANT=public"
set "FLIPPER_APPS="
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat"
cd /d "%~dp0"
del /q sdkconfig 2>nul
idf.py -B build_m5gen -DFLIPPER_BOARD=m5stick_s3 -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stick_s3" set-target esp32s3
echo === GEN_DONE ===
