#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test33.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 33 --firmware-number 12 --mode checkpoint5 \
  --notes "Test 32 (which ran the real kernel to checkpoint 1 on the phone) moved to checkpoint 5: XNU runs start.s through its own MMU and cache enable (SCTLR_EL1_DEFAULT) and returns from there, with Test 31's loader hand-off: firmware map fetched and validated but no per-region clean (R5Q_SKIP_MAP_CLEAN), ABL-style set/way clean+invalidate and ABL shutdown order (R5Q_ABL_SEQUENCE), coherent RAM-ring trace. Exact Test 17 BootShim and Test 12 firmware. 11 shim and 4 roundtrip Unicorn cases pass (Unicorn runs the actual EFI, the hand-off and XNU up to its MMU enable, its known limit); the r5q twin (QEMU, uH/TZ models) runs it to checkpoint 5 and back." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 32 has passed (it did, 2026-11-16 ~16:58)." | grep -E '"bundle"|"image"'
