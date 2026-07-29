@echo off
set "MSYSTEM="
set "IDF_TOOLS_PATH=C:\Users\stone\.espressif"
call "C:\Espressif\frameworks\esp-idf-v5.4.1\export.bat" > diag3.log 2>&1
echo ===AFTER_EXPORT_ERRLVL=%errorlevel%=== >> diag3.log
where idf.py >> diag3.log 2>&1
echo ===WHERE_ERRLVL=%errorlevel%=== >> diag3.log
where python >> diag3.log 2>&1
echo ===PATH_HEAD=%PATH:~0,400%=== >> diag3.log
