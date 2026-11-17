#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test93.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 93 --firmware-number 93 --mode diagnostic \
  --notes "Install update (2026-11-17) profile: iOS on core 7, the loader stays on core 0 watching it (run_on_other_cpu) with fastboot up the whole run (oem log = the kernel log so far, oem fwlog, oem stop, oem screenshot; phone.py live/stop/screenshot). upload refuses an empty stage (a DATA00000000 phase wedged Windows fastboot)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
