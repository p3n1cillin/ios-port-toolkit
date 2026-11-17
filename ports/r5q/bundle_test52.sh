#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test52.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 52 --firmware-number 52 --mode diagnostic \
  --notes "Display diagnosis (read-only). Under the full firmware (Tests 43, 51: the user saw iOS and the Apple logo shifted, at the panel's top right; each text line split) the panel shows the framebuffer from a shifted, drifting start; under the minimal firmware (Test 35) the picture was right. The stock bootloader sets up the panel the same way for both (its DisplayDxe log); the full firmware loads no display driver (SimpleFbDxe only), so the change is indirect (clocks/bus votes: underruns; or the IOMMU). Test 52 = the full firmware (fulldbg10) with a loader that only dumps MDP top, CTL0, DMA0, ping-pong 0, INTF 0/1, DSI0 and samples the MDP interrupt status (underruns) around one kick, then returns. Test 53 = the same dump under the minimal firmware." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 51 (2026-11-16 20:21)....." | grep -E '"bundle"|"image"'
