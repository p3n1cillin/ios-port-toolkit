#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test75.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 75 --firmware-number 75 --mode diagnostic \
  --notes "Install update (2026-11-17)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
