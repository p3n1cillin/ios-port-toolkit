#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test74.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 74 --firmware-number 74 --mode diagnostic \
  --notes "Install update (2026-11-17) keeps it there; a volume key held from the start counts too. With no PC the USB device is stopped again before iOS. Firmware fulldbg27: the early log in two slots, a start that died in the driver stage kept." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
