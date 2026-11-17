#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test75.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 75 --firmware-number 75 --mode diagnostic \
  --notes "Install update (2026-11-17)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
