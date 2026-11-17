#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test44.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 44 --firmware-number 44 --mode diagnostic \
  --notes "The filmed Test 43 showed the full firmware dashboard but its display scanout is offset, so the probe rows were off the panel; its cold reset wipes the RAM log. Storage works under the full firmware (the param flag was read and cleared through UFS). Test 44 = Test 43 + the boot manager copies the RAM log (DXE trail + probe results) into a log slot in the zero padding of the test recovery image at 0x4000000 (magic-guarded, only the GPT partition named recovery), after the probe and again before the reset; the controller reads the slot back from Android before restoring the original recovery." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after the filmed Test 43 (2026-11-16)." | grep -E '"bundle"|"image"'
