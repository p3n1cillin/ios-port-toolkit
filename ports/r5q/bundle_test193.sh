#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test193.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 193 --firmware-number 193 --mode diagnostic \
  --notes "Install update (2026-11-17) profile: iOS on core 7, the loader stays on core 0 watching it (run_on_other_cpu) with fastboot up the whole run (oem log = the kernel log so far, oem fwlog, oem stop, oem screenshot; phone.py live/stop/screenshot). upload refuses an empty stage (a DATA00000000 phase wedged Windows fastboot)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
