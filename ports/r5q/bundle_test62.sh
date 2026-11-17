#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test62.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 62 --firmware-number 62 --mode diagnostic \
  --notes "iOS with the display fixed. Test 61 (user: 'it's perfect'): real DSI reset (RESET 0x118) and the panel's write window resent (DCS 13, 2A 0-1079, 2B 0-2399 by command DMA from the bootloader's buffer) put the calibration pattern exactly on the panel. Test 62 = the Test 54 iOS run (iOS 16.5 system volume over the USB RAM drive, rebuilt at 0xFC000000, root.apfs md1, 240 s timed return) on firmware with that fix (resync before the console, ruler shown 3 s, autorefresh every 2nd TE as stock, no CTL_START kicks)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 61 (2026-11-16 21:46)........" | grep -E '"bundle"|"image"'
