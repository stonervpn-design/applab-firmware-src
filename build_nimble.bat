@echo off
setlocal
set "LOG=%~dp0build_nimble.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

>> "%LOG%" echo SENTINEL_SETTARGET
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" set-target esp32s3 >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_SETTARGET_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)

>> "%LOG%" echo SENTINEL_OK
:done
endlocal
