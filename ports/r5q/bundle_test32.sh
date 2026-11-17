#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test32.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 32 --firmware-number 12 --mode checkpoint1 \
  --notes "First real XNU entry with the hand-off that returned on hardware (Test 31). Test 15's payload and kernel (stop at early checkpoint 1, then return to the loader) with Test 31's loader hand-off: firmware map fetched and validated but no per-region clean (R5Q_SKIP_MAP_CLEAN), ABL-style set/way clean+invalidate and ABL shutdown order (R5Q_ABL_SEQUENCE), coherent RAM-ring trace. Exact Test 17 BootShim and Test 12 firmware. 11 shim and 4 roundtrip Unicorn cases pass (actual EFI -> hand-off -> XNU checkpoint 1 -> context return). Tests 15/16 (old hand-off) hung on the phone." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 31 has passed (it did, 2026-11-16 16:49)." | grep -E '"bundle"|"image"'
