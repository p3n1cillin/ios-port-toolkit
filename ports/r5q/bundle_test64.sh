#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test64.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 64 --firmware-number 64 --mode diagnostic \
  --notes "Touch, first run. Test 63: iOS home screen correctly aligned on the panel (user photo). Test 64 = Test 63 with the kernel xnu-r5q-touch.macho (sha 5443942f): the r5q port of redwood_sec_ts.c (kernel-build/r5q-overlay/bsd/dev): Samsung sec_ts (y771) at 0x48 on the GENI serial engine 0xc80000 (QUP wrapper 2 SE0; clocks voted in GCC 0x52014, branches 0x1e004/0x1e008/0x1e144, source 0x1e148), pins GPIO55/56 qup17 (SOUTH), interrupt GPIO87 (EAST), supplies GPIO131 (3.3 V) / GPIO140 (1.8 V) read and raised if low; steps logged; boot-args touchscreen=1 touch_log=1. The PC copies the system volume with unbuffered 8 MiB writes (buffered ran at 10.0 MiB/s)." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 63 (2026-11-16 22:20).........." | grep -E '"bundle"|"image"'
