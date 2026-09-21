#!/bin/bash
cd "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15"
cmd /c build_ci.bat 2>&1 | tee build_output.log
exit_code=${PIPESTATUS[0]}
echo "BUILD EXIT CODE: $exit_code"
exit $exit_code
