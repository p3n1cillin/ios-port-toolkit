#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
python3 package_test69.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 69 --firmware-number 69 --mode diagnostic \
  --notes "Test 68 (user): screen fills, touch accurate, time right, power works, volume does nothing (log: all three keys read and posted, backboardd got VolumeIncrement/Decrement: no audio device to change), a red rdar bar hides the battery (gauge read 100 %, 4270 mV), Clock black and Mail crashes, outer pages laggy, five fingers returned. Test 69 = Test 68 + root volume rebuilt as the donor's phone recipe does (REST=1: 382 more iOS jobs on demand, PosterBoard and AccessibilityUIServer angels) + kernel wifi_survey=1 (r5q: reads the SMEM at 0x86000000, the AP-modem partition and the modem's SMP2P items; nothing written) + firmware fulldbg23: SYSTEM.APF from a FAT microSD card when SYSTEM.CRC matches (else USB as before), and the epoch from the PMIC RTC when no PC gave one." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 68 (2026-11-16 23:00)..........." | grep -E '"bundle"|"image"'
