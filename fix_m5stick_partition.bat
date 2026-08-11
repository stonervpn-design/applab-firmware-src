@echo off
setlocal
REM Fix: build_m5stick_s3 was configured with partitions_singleapp_large.csv (no /ext
REM storage partition). Delete the pinned sdkconfig so reconfigure regenerates it from
REM sdkconfig.defaults.m5stick_s3 (which sets partitions_m5stick_s3.csv -> storage@0x390000).
REM Reuses compiled objects (sources unchanged); only regenerates config + relinks.
set "LOG=%~dp0fix_m5stick_partition.log"
set "MSYSTEM="
set "MINGW_PREFIX="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
set "FLIPPER_BOARD=m5stick_s3"
set "SDKDEF=sdkconfig.defaults;sdkconfig.defaults.m5stick_s3"
> "%LOG%" echo SENTINEL_START
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_EXPORT_FAIL & goto :done)
cd /d "%~dp0"

REM force sdkconfig regeneration from defaults (the wrong partition table lived here)
if exist "build_m5stick_s3\sdkconfig" del /f /q "build_m5stick_s3\sdkconfig"

>> "%LOG%" echo SENTINEL_RECONFIG
idf.py -B build_m5stick_s3 -DFLIPPER_BOARD=m5stick_s3 -DSDKCONFIG_DEFAULTS="%SDKDEF%" reconfigure build >> "%LOG%" 2>&1
if errorlevel 1 (>> "%LOG%" echo SENTINEL_BUILD_FAIL & goto :done)
>> "%LOG%" echo SENTINEL_OK
:done
endlocal
