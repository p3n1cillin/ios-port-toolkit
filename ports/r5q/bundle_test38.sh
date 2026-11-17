#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test38.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 38 --firmware-number 38 --mode diagnostic \
  --notes "Test 37 (full r5q firmware) boot-looped on the phone: every loop asserted in a Qualcomm binary DXE, UncachedMemoryAllocationLib +389 Out of Resources, because Silicium adds the RAM partitions (RamManagerDxe) only after all APRIORI drivers, so the Qualcomm drivers start with the 28 MiB DXE heap. Test 38 = Test 37 with RamManagerDxe in APRIORI right after TimerDxe (EnvDxe, which provides the RAM-partition protocol, is earlier). Same read-only probe loader." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 37 boot-looped (2026-11-16 ~17:55)." | grep -E '"bundle"|"image"'
