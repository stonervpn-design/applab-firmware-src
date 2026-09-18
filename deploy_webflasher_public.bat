@echo off
setlocal
REM One-shot SAFE build+stage of the public webflasher bin. This is THE script to use
REM for any webflasher update. It ALWAYS regenerates the sdkconfig (build_only_public
REM deletes the shared sdkconfig so it re-seeds from the m5stick defaults) and HARD-
REM VERIFIES the 'storage' partition at every stage, so a wrong-board build (no PSRAM /
REM no storage partition = "mounting sd card failed" + no /ext) can NEVER reach
REM webflash\. After it succeeds: bump webflash\manifest.json "version", then
REM git add/commit/push inside webflash\.
set "DIR=%~dp0"
cd /d "%DIR%"

echo [1/5] Building firmware (regenerates sdkconfig from the m5stick defaults)...
call "%DIR%build_only_public.bat"
findstr /C:"SENTINEL_OK" "%DIR%build_only_public.log" >nul
if errorlevel 1 (echo   FAILED: firmware build did not reach SENTINEL_OK. See build_only_public.log & goto :fail)

echo [2/5] Verifying built partition table has 'storage'...
python "%DIR%tools\verify_partition_table.py" "%DIR%build_m5stick_s3_public\partition_table\partition-table.bin"
if errorlevel 1 goto :fail

echo [3/5] Regenerating /ext Manifest...
python "%DIR%tools\gen_ext_manifest.py" "%DIR%_extfs_stage"
if errorlevel 1 (echo   FAILED: gen_ext_manifest. & goto :fail)

echo [4/5] Baking /ext + merging...
call "%DIR%build_ext_and_merge_public.bat"
findstr /C:"SENTINEL_OK" "%DIR%build_ext_and_merge_public.log" >nul
if errorlevel 1 (echo   FAILED: ext/merge did not reach SENTINEL_OK. See build_ext_and_merge_public.log & goto :fail)

echo [5/5] Verifying MERGED bin + staging to webflash\...
python "%DIR%tools\verify_partition_table.py" "%DIR%Flipper-m5stick_s3-public-merged.bin"
if errorlevel 1 (echo   Merged bin missing 'storage' - NOT staged. & goto :fail)
echo [verify] car-key guard (private-only): bin must be car-free...
python "%DIR%tools\verify_public_no_car.py" "%DIR%Flipper-m5stick_s3-public-merged.bin"
if errorlevel 1 (echo   ABORT: car key material found in bin - NOT staged. & goto :fail)
echo [verify] car-key guard: webflash\sd_content (SD download) must be car-free too...
REM The bin guard above only scans the firmware image. sd_content\ is a SEPARATE public
REM output (the microSD download) and once nearly leaked a stale VAG keystore there, so
REM scan it as well. --skip-keeloq: keeloq_mfcodes_user is dual-use gate/garage codes and
REM legitimately ships; the bin scan keeps the full keeloq check.
python "%DIR%tools\verify_public_no_car.py" --skip-keeloq "%DIR%webflash\sd_content"
if errorlevel 1 (echo   ABORT: car material found in webflash\sd_content - NOT staged. & goto :fail)
copy /Y "%DIR%Flipper-m5stick_s3-public-merged.bin" "%DIR%webflash\Flipper-m5stick_s3-public-merged.bin" >nul

echo [changelog] posting public-build changelog to webhook (best-effort)...
python "%DIR%tools\public_changelog.py" --board m5stick
if errorlevel 1 echo   (changelog webhook post failed - deploy still OK)

echo.
echo SUCCESS: webflash\Flipper-m5stick_s3-public-merged.bin staged and verified (storage present).
echo Next: bump webflash\manifest.json "version", then in webflash\:  git add -A ^&^& git commit ^&^& git push
goto :done

:fail
echo.
echo DEPLOY ABORTED - nothing was staged to webflash\.
endlocal
exit /b 1

:done
endlocal
