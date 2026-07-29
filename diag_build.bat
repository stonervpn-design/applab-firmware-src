@echo off
set "MSYSTEM="
set "MINGW_PREFIX="
set "MSYS2_PATH_TYPE="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
set "PYTHONIOENCODING=utf-8"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" >nul 2>&1
cd /d "%~dp0"
echo ===NINJA_TARGETS_GREP===
ninja -C build_cardputer_adv -t targets all 2>nul | findstr /I "wlan_hal.c.obj scene_ssid_spam_run"
echo ===NINJA_EXPLAIN_DRYRUN===
ninja -C build_cardputer_adv -n -d explain furi_esp32.elf 2>&1 | findstr /I "wlan_hal scene_ssid_spam explain ninja:"
echo ===DONE===
