#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test60.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 60 --firmware-number 60 --mode diagnostic \
  --notes "Display alignment. Test 59 video (user): the panel's top half shows our ruler from row ~1200 down to the bottom border; the bottom half black, no magenta: the pipe fetches our framebuffer correctly; the panel starts each frame's write ~1200 rows in (Test 56: ~580): per-boot panel state. Test 60 = Test 59 + R5qPanelWindow in the resync: DCS 13 (normal mode), 2A 00 00 04 37, 2B 00 00 09 5F (the stock bootloader's init values) sent through the DSI command DMA in Linux msm's packet format (DMA_BASE 0x48, DMA_LEN 0x4C, TRIG_DMA 0x90, CMD_DMA_DONE in INTR_CTRL 0x110), buffer at 0x9E200000; then the clean frame, autorefresh, ruler." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 59 (2026-11-16 21:30)..........." | grep -E '"bundle"|"image"'
