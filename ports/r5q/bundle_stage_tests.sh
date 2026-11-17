#!/bin/bash
# Package and bundle Tests 19-23 as guarded, UNFLASHED hardware tests.
set -eu
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
cd "$R"
declare -A WHAT=(
 [19]="stops in C after the full cache clean, just before the MMU-off jump (MMU on)"
 [20]="stops in the return stub right after RB0, the first step with the MMU and caches off"
 [21]="stops after RB2: translation registers reloaded, TLB and I-cache invalidated, MMU still off"
 [22]="stops after RB3: right after the MMU is turned back on"
 [23]="no reset: the full return (Test 18) plus RB1-RB4 stage tags")
for n in 19 20 21 22 23; do
	python3 package_test$n.py "$R" | grep -E '"sha256"' | head -1
	reset="then resets with the firmware's own PS_HOLD write (0x0C264000) so the phone returns to Android by itself"
	[ $n = 23 ] && reset="it may hang like Test 18; run it only after 19-22 return"
	python3 bundle_native_test.py $n --firmware-number 12 --mode return-selftest \
	  --notes "Return-path stage bisection, no XNU entry. Test 18 parts (exact Test 17 BootShim, exact Test 12 firmware, coherent-trace return-selftest loader) plus cache-maintained RB0-RB4 stub tags. This one ${WHAT[$n]}; $reset. 11 shim and 4 roundtrip Unicorn cases pass; real hardware behaviour is unproven." \
	  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run 19, 20, 21, 22, 23 in order and stop at the first that does not return." | grep -E '"bundle"' 
done
