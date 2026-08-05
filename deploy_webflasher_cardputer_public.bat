@echo off
setlocal
REM Build + stage the PUBLIC Cardputer ADV webflasher bin, then post a changelog.
REM Mirrors deploy_webflasher_public.bat (m5stick) but for the Cardputer public
REM webflasher. Cardputer-ADV uses a real SD card, so there is no /ext bake step.
REM PUBLIC ONLY - the changelog webhook is never fired for private builds.
set "DIR=%~dp0"
cd /d "%DIR%"

echo [1/2] Building PUBLIC Cardputer ADV firmware...
call "%DIR%build_adv_public.bat"
findstr /C:"SENTINEL_OK" "%DIR%build_cardputer_adv_public.log" >nul
if errorlevel 1 (echo   FAILED: build did not reach SENTINEL_OK. See build_cardputer_adv_public.log & goto :fail)

echo [verify] car-key guard (private-only): bin must be car-free...
python "%DIR%tools\verify_public_no_car.py" "%DIR%Flipper-cardputer_adv-public-merged.bin"
if errorlevel 1 (echo   ABORT: car key material found in bin - NOT staged. & goto :fail)

echo [2/2] Staging merged bin to webflash_cardputer_adv\...
copy /Y "%DIR%Flipper-cardputer_adv-public-merged.bin" "%DIR%webflash_cardputer_adv\Flipper-cardputer_adv-public-merged.bin" >nul
if errorlevel 1 (echo   FAILED: could not stage merged bin. & goto :fail)

echo [changelog] posting public Cardputer changelog to webhook (best-effort)...
python "%DIR%tools\public_changelog.py" --board cardputer
if errorlevel 1 echo   (changelog webhook post failed - deploy still OK)

echo.
echo SUCCESS: webflash_cardputer_adv\Flipper-cardputer_adv-public-merged.bin staged.
echo Next: bump webflash_cardputer_adv\manifest.json "version", then in webflash_cardputer_adv\:  git add -A ^&^& git commit ^&^& git push
goto :done

:fail
echo.
echo DEPLOY ABORTED - nothing staged for the Cardputer public webflasher.
endlocal
exit /b 1

:done
endlocal
