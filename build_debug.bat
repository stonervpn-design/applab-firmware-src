@echo on
REM Debug build script
cd /d "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15"
echo === Current directory: %cd% ===
echo === Setting environment variables ===
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
echo === Calling ESP-IDF export ===
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat"
if %errorlevel% neq 0 (
    echo === EXPORT FAILED WITH CODE %errorlevel% ===
    exit /b 90
)
echo === Checking idf.py ===
where idf.py
if %errorlevel% neq 0 (
    echo === idf.py NOT FOUND ===
    exit /b 91
)
echo === Environment is ready ===
echo === Running idf.py reconfigure ===
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" reconfigure build
echo === Build complete with code %errorlevel% ===
exit /b %errorlevel%
