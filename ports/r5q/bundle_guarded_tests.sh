#!/bin/bash
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
declare -A BASE=([25]=20 [26]=21 [27]=22 [28]=23)
declare -A WHAT=(
 [25]="stops right after the MMU and caches go off (RB0)"
 [26]="stops after the translation registers are reloaded and the caches cleared (RB2)"
 [27]="stops right after the MMU is turned back on (RB3)"
 [28]="runs the full return with stage tags, like Test 23")
for n in 25 26 27 28; do
	python3 package_test$n.py "$R" | grep -E '"sha256"' | head -1
	reset="then resets with the firmware's PS_HOLD write so the phone returns to Android by itself"
	[ $n = 28 ] && reset="returns through the firmware, which resets via PS_HOLD"
	python3 bundle_native_test.py $n --firmware-number 12 --mode return-selftest \
	  --notes "Watchdog-guarded copy of Test ${BASE[$n]}, no XNU entry. Identical to Test ${BASE[$n]} except the APSS watchdog (0x17C10000) is armed (bark 5 s, bite 8 s, Samsung watchdog_v2.c layout) just before the MMU-off step, so a hang in the hand-off reboots the phone by itself (debug level LOW). This one ${WHAT[$n]}; $reset. 11 shim and 4 roundtrip Unicorn cases pass. Run only after Test 24 confirmed a clean watchdog bite." \
	  --start-blocked-reason "Built and checked host-side. Needs an explicit user OK per run, and Test 24 must have shown a clean watchdog reboot first." | grep -E '"bundle"'
done
