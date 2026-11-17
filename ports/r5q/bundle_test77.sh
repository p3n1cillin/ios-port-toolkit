#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test77.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 77 --firmware-number 77 --mode diagnostic \
  --notes "Install update (2026-11-17); loader reads volume keys from every text input (ConIn is never connected) and waits 5 s for a USB connection." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
