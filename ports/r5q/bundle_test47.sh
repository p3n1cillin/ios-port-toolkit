#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test47.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 47 --firmware-number 47 --mode diagnostic \
  --notes "Test 46: Qualcomm UsbMsd located (rev 0x10001, 16 LUNs), RAM disk assigned to LUN 0, but StartDevice = NOT_FOUND: UsbMsdDxe itself logs Failed to locate USB FN IO protocol. UsbfnDwc3Dxe installs UsbfnIo only when connected to the USB core handle, which a normal BDS does with connect-all and our boot manager never did. Test 47 = Test 46 + EfiBootManagerConnectAll() before the USB RAM drive session (logs the UsbfnIo handle count before and after, and saves the log slot after it). Otherwise unchanged: only a 3 GiB FAT32 RAM disk is assigned to USB." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 46 (2026-11-16 19:35).." | grep -E '"bundle"|"image"'
