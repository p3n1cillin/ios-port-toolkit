#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test35.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 35 --firmware-number 12 --mode timed-run \
  --notes "Test 34 (25 s timed kernel run, no -v) hung on the phone with no visible kernel output. Test 35 is the same run with -v: the kernel prints its boot on the phone's screen (the loader leaves the text framebuffer to it), with a 60 s timed return; if it hangs, the last lines on screen say where. Same Test 31 hand-off, shim, firmware and kernel. Unicorn: EFI, hand-off and XNU to its MMU enable; twin: the whole -v run." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 34 hung (2026-11-16 ~17:15) to see where." | grep -E '"bundle"|"image"'
