#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test56.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 56 --firmware-number 56 --mode diagnostic \
  --notes "Display alignment. Test 55 photo (user): the calibration frame lands ~620 rows + 90 px early on the panel: centre cross at a quarter of the height, bottom border at ~72%, the top ~620 rows (top border, red/green corners) missing, the panel's last rows still the stock splash ('software updates.', 'Powered by android', correctly placed), left border wrapped to ~92% of the width. The resync drained (line 960) and sent one clean frame, so the out-of-phase state is the DSI controller's command-stream counter. Test 56 = Test 55 + DSI soft reset inside the resync (Linux msm dsi_sw_reset_restore: CTRL enable off, RESET 0x114 pulsed 20 ms, CTRL restored), with autorefresh paused and nothing in flight, then the clean frame and the pattern." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 55 (2026-11-16 20:58)......." | grep -E '"bundle"|"image"'
