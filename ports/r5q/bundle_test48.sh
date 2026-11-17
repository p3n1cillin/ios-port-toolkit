#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test48.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 48 --firmware-number 48 --mode diagnostic \
  --notes "Test 47: connect-all ran (86 ms) but UsbfnIo handles stayed 0 -> 0; UsbMsdDxe StartDevice NOT_FOUND again. UsbConfigDxe starts the USB cores only on the gUsbControllerInitGuid event group (1C0CFFCE-FC8D-4E44-8C78-9C9E5B530D36), which Qualcomm's DeviceBootManagerOnDemandConInConnect signals and our Null device boot manager never did. Test 48 = Test 47 + signal that event group, then connect all and poll up to 5 s for UsbfnIo, then the USB RAM drive session (only a 3 GiB FAT32 RAM disk is assigned to USB)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 47 (2026-11-16 19:39)..." | grep -E '"bundle"|"image"'
