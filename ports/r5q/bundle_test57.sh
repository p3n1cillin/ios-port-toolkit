#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test57.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 57 --firmware-number 57 --mode diagnostic \
  --notes "Display alignment. Test 56 photo: the DSI soft reset fixed the horizontal shift completely (blue/yellow squares and the right border on the edges); a pure vertical offset remains, the same ~620 rows as Test 55: the frame's top ~620 rows never reach the panel, whose last ~620 rows keep the stock splash. Suspect: autorefresh on every TE pulse (0x80000001, set by BootShim) where Qualcomm's DisplayDxe uses every 2nd (AutoRefreshFrameNumDiv 2); a 1080x2400 transfer takes about one 60 Hz period. Test 57 = Test 56 + the calibration pattern shown in three modes, 8 s each, numbered by 1-3 white squares left of the cross: (1) autorefresh off, one clean frame; (2) every 2nd TE; (3) every TE. Mode 2 stays on after." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 56 (2026-11-16 21:05)........" | grep -E '"bundle"|"image"'
