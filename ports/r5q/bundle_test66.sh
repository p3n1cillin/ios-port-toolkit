#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test66.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 66 --firmware-number 66 --mode diagnostic \
  --notes "Test 65 (user): still laggy but better, Wi-Fi in Settings looks stuck (no WLAN driver yet), battery shows near zero (no fuel gauge driver yet), the time is wrong (the epoch boot-arg was the bundle time). Test 66 = Test 65 + the CPU clock (kernel redwood_cpufreq ported to the SM8150 OSM blocks 0x18323000/0x18325800/0x18327800: each cluster to its top LUT row; boot-arg cpufreq=1; no DDR vote on r5q) + the time (the PC writes EPOCH.TXT before DONE.TXT; firmware fulldbg22 overwrites the 10 epoch digits in the payload cmdline with the PC clock plus the seconds since, just before the loader)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 65 (2026-11-16 22:50)..........." | grep -E '"bundle"|"image"'
