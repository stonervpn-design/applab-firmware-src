@echo off
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" > diag2.log 2>&1
cd /d "%~dp0"
echo ===EXPORT_ERRLVL=%errorlevel%=== >> diag2.log
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" build >> diag2.log 2>&1
echo ===BUILD_ERRLVL=%errorlevel%=== >> diag2.log
