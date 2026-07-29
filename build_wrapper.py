#!/usr/bin/env python3
import subprocess
import os
import sys

os.chdir(r'C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15')

# Set environment variables
os.environ['MSYSTEM'] = ''
os.environ['MINGW_PREFIX'] = ''
os.environ['MSYS2_PATH_TYPE'] = ''
os.environ['IDF_TOOLS_PATH'] = r'C:\Users\stone\.espressif'

print("=== Python Build Wrapper ===")
print(f"Working directory: {os.getcwd()}")
print(f"IDF_TOOLS_PATH: {os.environ.get('IDF_TOOLS_PATH')}")

# Try to run the batch file with full path
bat_path = os.path.join(os.getcwd(), 'build_ci.bat')
print(f"\n=== Running build_ci.bat: {bat_path} ===")
if not os.path.exists(bat_path):
    print(f"ERROR: {bat_path} not found!")
    sys.exit(1)

result = subprocess.run(['cmd.exe', '/c', bat_path], cwd=os.getcwd())
print(f"\n=== Build completed with exit code: {result.returncode} ===")
sys.exit(result.returncode)
