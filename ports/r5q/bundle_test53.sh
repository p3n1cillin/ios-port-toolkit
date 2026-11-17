#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test53.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 53 --firmware-number 53 --mode diagnostic \
  --notes "Display diagnosis (read-only), the reference half: the MDSS dump loader of Test 52 under the MINIMAL firmware (Test 36's FD, r5qtestPkg), where the panel showed the picture correctly (Test 35). Diff with Test 52." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 52 (2026-11-16)....." | grep -E '"bundle"|"image"'
