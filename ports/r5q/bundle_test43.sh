#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test43.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 43 --firmware-number 43 --mode diagnostic \
  --notes "The filmed Test 42 showed the FULL r5q firmware working on the phone: all of DXE, BDS, our boot manager, the probe loader run and returned (LOADER CHECK PASSED, 8 s countdown); only the final PS_HOLD reset is cold under this firmware (Qualcomm PMIC driver), which wipes the RAM log. Test 43 = Test 42 + the full firmware boot manager copy (r5qfullPkg) drawing the probe results in its large dashboard font: USB device protocol, block devices, free RAM and the largest block." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after the filmed Test 42 (2026-11-16 ~18:45)." | grep -E '"bundle"|"image"'
