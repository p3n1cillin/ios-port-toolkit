#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test54.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 54 --firmware-number 54 --mode diagnostic \
  --notes "Display fix. Tests 52/53 (read-only MDSS dumps, full vs minimal firmware): identical configuration, no underruns, autorefresh on in both; the Test 35 photo (minimal firmware) shows the same shift, constant (3 characters, each row's start wrapped to the right edge), so the cause is not the full firmware: the stock bootloader ('Could not get splash memory region node') leaves the panel's pipe fetching its own buffer, which differs from 0x9C400000 (where UEFI's GOP, the boot manager and iOS draw) by an amount that varies per boot; BootShim's bands, drawn at the scanned address, were always right. Test 54 = Test 51 (iOS 16.5 system volume over the USB RAM drive, 240 s) + R5qFixScanout in the boot manager's BeforeConsole: logs BootShim's record, the CTL path and every DRAM-fetching pipe, points them at 0x9C400000 and flushes their CTL bits (taken at the next frame)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Tests 52/53 (2026-11-16 20:28)......." | grep -E '"bundle"|"image"'
