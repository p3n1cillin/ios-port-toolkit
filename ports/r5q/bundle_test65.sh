#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test65.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 65 --firmware-number 65 --mode diagnostic \
  --notes "Touch works (Test 64, user: swipe works, laggy and delayed; opening Settings got stuck). Log: the chip's supplies were off (GPIO131/140 raised), the engine's FIFO is GSI-only so the bus is bit-banged at 100 kHz (reads 1.4 ms, fine in the 8 ms poll), the chip id ac 37 71, application running; Settings spawned 15 s after the tap: iOS ran on one core (cpus=1) at the boot clock 1.48 GHz with software rendering. PC copy still 10.0 MiB/s unbuffered: the limit is the phone side. Test 65 = Test 64 with all 8 cores: loader R5Q_LAB_SMP accepts cpus=N in the lab profile (PSCI AFFINITY_INFO probe, Aff1 numbering, the kernel starts them with CPU_ON and stops them at its return); cmdline cpus=8." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 64 (2026-11-16 22:35)..........." | grep -E '"bundle"|"image"'
