#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test39.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 39 --firmware-number 39 --mode diagnostic \
  --notes "Test 37 (full r5q firmware) asserted Out of Resources in a Qualcomm DXE on every loop; Test 38 (RamManagerDxe in APRIORI) powered the phone off with no log. Working SM8150 Silicium ports (Xiaomi Pad 5 nabu, LG G8x mh2) keep RamManagerDxe out of APRIORI and have a 60 MiB DXE heap; the r5q port has 28 MiB. Test 39 = Test 37 (upstream APRIORI) + a second 60 MiB conventional DXE heap at 0xD8000000 (free DRAM between camera_mem_region and the test window), own MemoryMapLib copy. Same read-only probe loader." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 38 powered off (2026-11-16 ~18:00)." | grep -E '"bundle"|"image"'
