#!/bin/bash
set -e

echo "=== Setting up ESP-IDF environment ==="
. /c/Espressif/frameworks/esp-idf-v5.4.1/export.sh

cd /c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15

echo "=== set-target ==="
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" set-target esp32s3

echo "=== build ==="
idf.py -B build_cardputer_adv -DFLIPPER_BOARD=m5stack_cardputer_adv -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stack_cardputer_adv" reconfigure build

echo "=== merge_bin ==="
python -m esptool --chip esp32s3 merge_bin -o "Flipper-cardputer_adv-merged.bin" \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 "build_cardputer_adv/bootloader/bootloader.bin" \
  0x8000 "build_cardputer_adv/partition_table/partition-table.bin" \
  0x10000 "build_cardputer_adv/furi_esp32.bin"

echo "=== BUILD OK ==="
