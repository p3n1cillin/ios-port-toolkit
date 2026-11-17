#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test37.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 37 --firmware-number 37 --mode diagnostic \
  --notes "First run of the FULL r5q firmware on the phone: r5qfullPkg = r5qPkg with every Qualcomm DXE (TZ, SMEM, clocks, RPMh, PMIC, SPMI, I2C, TLMM, UFS, display, USB function/device/config, PIL, ...) plus the hardware-proven Test 12 boot manager and memory map. The Test 17 BootShim starts it; the boot manager runs a read-only probe loader that reports the Qualcomm USB device protocol (needed for loading the iOS system image over USB), the block devices (UFS) and the conventional RAM, then returns; PS_HOLD reset. No XNU entry." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 36 (XNU boots fully, 2026-11-16 ~17:34)." | grep -E '"bundle"|"image"'
