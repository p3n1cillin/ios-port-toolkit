#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test72.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 72 --firmware-number 72 --mode diagnostic \
  --notes "Install update (2026-11-16), offers a failed start's log over USB on the next start (collect_phone_logs.py), and does the power-off itself after saving (loader returns POFF)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Install update (2026-11-16)..........." | grep -E '"bundle"|"image"'
