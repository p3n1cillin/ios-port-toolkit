#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test55.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 55 --firmware-number 55 --mode diagnostic \
  --notes "Display alignment. Test 54's log: the pipe already fetched exactly 0x9C400000 (vig0, 1080x2400, stride 4320, no crop, ARGB8888); the DSI inserts write_memory_start each frame with a 1080x2400 stream; still the picture was displaced with wraparound by a per-boot amount, while Android renders perfectly on the same panel (user). So frames and the panel's frame start are out of phase; our BootShim and boot manager fired CTL_START while autorefresh was sending frames, which Qualcomm's driver never does. Test 55 = full firmware with R5qPanelResync (autorefresh off, transfer drained, one clean frame, autorefresh back on), KickDisplay without CTL_START (and no longer writing INTF0), and a 15 s calibration pattern (white edge border, red/green/blue/yellow corners, centre cross) for a photo; then Test 50's kernel run (root.apfs, 90 s). No USB stage." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 54 (2026-11-16 20:48)......" | grep -E '"bundle"|"image"'
