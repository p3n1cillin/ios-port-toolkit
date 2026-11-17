#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test71.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 71 --firmware-number 71 --mode diagnostic \
  --notes "Installed BOOT update: Test 70 + the loader honours iOS's power-off request (lab return path: kernel_power == off -> the firmware's ResetSystem(EfiResetShutdown)) instead of resetting into iOS again." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
