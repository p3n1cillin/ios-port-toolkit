#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test49.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 49 --firmware-number 49 --mode diagnostic \
  --notes "First iOS-root run on the full r5q firmware. Test 48 proved PC-to-phone-RAM over USB; Test 36 proved XNU to the root wait under the minimal firmware. Test 49 = the full firmware (USB RAM drive compiled out) running the lab loader with R5Q_LAB_USERLAND: Test 36's phone-proven hand-off (ABL sequence, no per-region map clean, gic_vtimer=19) but the window is the largest free block (0xF4000000, 6 GiB, the production arithmetic), the payload carries root.apfs (4 MiB, the iOS 16.5 root volume of the twin build) as the ramdisk (rd=md0), 90 s timed return without -v; the kernel log comes back into the RAM ring (last 1 MiB, off the text console) and the log slot. No system volume yet (Test 50: system.apfs over the USB RAM drive)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 48 (2026-11-16 19:38)...." | grep -E '"bundle"|"image"'
