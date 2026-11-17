#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test40.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 40 --firmware-number 40 --mode diagnostic \
  --notes "Observability run for the full r5q firmware, which Tests 37-39 could not get running (37: Out of Resources assert loop; 38: cold power-off; 39: stuck at Samsung splash, no trace). Test 40 = Test 39 (upstream APRIORI, extra 60 MiB DXE heap) built as a DEBUG target: every DEBUG message, including the DXE core line for each driver it loads, is drawn on the panel (Silicium FrameBufferSerialPortLib) and appended to the Samsung RAM ring (r5qfull RingFbSerialPortLib), so a photo of the screen or last_kmsg shows where it stops. Same read-only probe loader." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 39 stuck with no trace (2026-11-16 ~18:10)." | grep -E '"bundle"|"image"'
