#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test34.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 34 --firmware-number 12 --mode timed-run \
  --notes "Real XNU with no checkpoint after Tests 32/33 passed on the phone: the kernel boots as far as it can for 25 s (boot-arg loader_return_after=25, no -v) and returns to the loader by its own timer (or by its panic path); the loader prints the kernel log (up to 1 MiB) into the Samsung RAM ring, so last_kmsg shows how far XNU got on the phone. Test 31 hand-off; reserved 256 MiB window at 0xE0000000; no ramdisk, so a healthy run ends waiting for a root device. Unicorn runs the EFI, the hand-off and XNU to its MMU enable (its limit); the r5q twin runs the whole timed boot." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 33 has passed (it did, 2026-11-16 ~17:05)." | grep -E '"bundle"|"image"'
