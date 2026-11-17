#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test42.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 42 --firmware-number 42 --mode diagnostic \
  --notes "Test 41 (DEBUG full firmware, secure no-map regions reserved) reset back to Android within ~30 s with no trace (a cold reset wipes the RAM ring). The user could not read the panel trail in Test 40 (lines ran under the camera cut-out and off both edges). Test 42 = Test 41 with the r5q debug writer drawing inside a safe area (48 px side margins, 180 px top) and flushing only the rows it touched, so the last lines before a reset can be filmed. Same firmware configuration and probe loader." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 41 reset with no trace (2026-11-16 ~18:26)." | grep -E '"bundle"|"image"'
