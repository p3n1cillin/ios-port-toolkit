#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test70.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 70 --firmware-number 70 --mode diagnostic \
  --notes "The install build (user approved the full install, 2026-11-16 ~23:25): BOOT gets this image; firmware fulldbg24 loads iOS from userdata when installed, else takes it over USB and, with usb_msd_session.py --install, writes it to userdata and reads it back. Test 69 kernel, root volume REST=1 + .GlobalPreferences UIStatusBarVisualProviderClassName=_UIStatusBarVisualProvider_Split1125 (the red rdar bar: UIKit's fallback for the 375x812@2x screen of fbscale=2), loader 86400 s." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install (2026-11-16)..........." | grep -E '"bundle"|"image"'
