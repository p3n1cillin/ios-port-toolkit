#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test63.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 63 --firmware-number 63 --mode diagnostic \
  --notes "Test 62: display fix applied (13/2A/2B sent), but 'SYSTEM.APF: no memory ... at the resident address': the panel command page claimed at the bootloader's buffer 0xFF30A000 lies inside the resident range 0xFC000000-0x1908C0000 and was never released, so the system image could not be placed and iOS did not boot (user: 'it never booted to ios'). Test 63 = Test 62 with that page freed right after the panel commands (no clear-on-free: PcdDebugPropertyMask 0x27)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 62 (2026-11-16 22:00)........." | grep -E '"bundle"|"image"'
