#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test71.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 71 --firmware-number 71 --mode diagnostic \
  --notes "Installed BOOT update: Test 70 + the loader honours iOS's power-off request (lab return path: kernel_power == off -> the firmware's ResetSystem(EfiResetShutdown)) instead of resetting into iOS again." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
