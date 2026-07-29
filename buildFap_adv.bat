@echo off
REM Build a .fap for the Cardputer-ADV. ESP-IDF refuses to run under git-bash, so set
REM up the toolchain in cmd.exe (export.bat), find its bin dir, convert to a git-bash
REM path, and hand it to buildFap.sh via FAP_TOOLCHAIN_BIN (it prepends it to PATH).
REM Usage:  buildFap_adv.bat applications_user/nRF24_jammer
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat"
if %errorlevel% neq 0 (echo === EXPORT FAILED === & exit /b 90)
cd /d "%~dp0"
for /f "delims=" %%i in ('where xtensa-esp32s3-elf-gcc') do set "GCC_EXE=%%i"
if not defined GCC_EXE (echo === TOOLCHAIN NOT ON PATH === & exit /b 91)
for %%d in ("%GCC_EXE%") do set "GCC_DIR=%%~dpd"
REM C:\...\bin\  ->  /C/.../bin/
set "U=%GCC_DIR:\=/%"
set "U=%U::=%"
set "FAP_TOOLCHAIN_BIN=/%U%"
echo === toolchain bin: %FAP_TOOLCHAIN_BIN% ===
bash buildFap.sh %1
echo FAP_BUILD_EXIT=%errorlevel%
