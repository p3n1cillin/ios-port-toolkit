#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test24.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 24 --firmware-number 12 --mode return-selftest \
  --notes "APSS watchdog probe, no MMU switch and no XNU entry. Test 19's path up to the hand-off, then the loader arms the APSS watchdog at 0x17C10000 exactly as Samsung's watchdog_v2.c does (bark 5 s, bite 8 s at 32765 Hz) and never pets it. A bite should reboot to Android (debug level LOW: Samsung clears download mode and sets restart reason NORMAL on every normal reboot). If no reset comes within 20 s by CNTVCT, it falls back to the firmware's PS_HOLD write, so the run returns either way; the log shows which happened. 11 shim and 5 roundtrip Unicorn cases pass. If it bites cleanly, an armed watchdog can guard Tests 20-23 so any hang reboots by itself." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 19 has returned." | grep -E '"bundle"'
