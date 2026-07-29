@echo off
setlocal
REM Incremental build ONLY (no set-target) for the PRIVATE variant into build_m5stick_s3.
REM Skips idf.py set-target (Python 3.14 component-manager HTTPStatus crash). Private =
REM full features: FLIPPER_VARIANT=private (fam_config keeps all apps) and NO
REM -DFLIPPER_VARIANT=public, so PUBLIC_BUILD stays off. Mirrors build_only_public.bat.
set "LOG=%~dp0build_only_private.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
set "FLIPPER_VARIANT=private"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stick_s3"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"
REM Regenerate the shared sdkconfig from the m5stick defaults (see build_only_public.bat).
if exist "%~dp0sdkconfig" del /f /q "%~dp0sdkconfig" >> "%LOG%" 2>&1
>> "%LOG%" echo SENTINEL_BUILD
idf.py -B build_m5stick_s3 -DFLIPPER_BOARD=m5stick_s3 -DSDKCONFIG_DEFAULTS="%SDKDEF%" build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
