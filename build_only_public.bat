@echo off
setlocal
REM Incremental build ONLY (no set-target) into build_m5stick_s3_public.
REM Skips idf.py set-target (Python 3.14 component-manager HTTPStatus crash).
set "LOG=%~dp0build_only_public.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
set "FLIPPER_VARIANT=public"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stick_s3"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
REM CRITICAL: the root sdkconfig is SHARED across boards and is clobbered whenever a
REM different board (cardputer) is built. Skipping set-target means we must delete it
REM so the build re-seeds from the m5stick defaults; otherwise the m5stick firmware
REM inherits the wrong config (no PSRAM, no storage partition = SD mount fail, no /ext).
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_m5stick_s3_public -DFLIPPER_BOARD=m5stick_s3 -DFLIPPER_VARIANT=public -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
