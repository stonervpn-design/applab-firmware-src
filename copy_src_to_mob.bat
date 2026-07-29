@echo off
robocopy "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15" "C:\Users\stone\OneDrive\Desktop\MOB\Flipper-Zero-meets-M5Stack-Cardputer-1.15" /E /XD build_cardputer_adv build_m5stick_s3 managed_components .git /XF *.log /R:1 /W:1 /NP /NFL /NDL
echo ROBOCOPY_EXIT=%errorlevel%
