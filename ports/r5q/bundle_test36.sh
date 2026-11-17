#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test36.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 36 --firmware-number 12 --mode timed-run \
  --notes "Fix for Tests 34/35: on the phone XNU booted (photo: verbose boot through BSD to MiniIOMobileFramebuffer) but its timer never fired, because its GIC driver hard-codes the virtual timer at INTID 27 while SM8150 wires it to INTID 19 (Android DT <1 3>). Test 36 = Test 35 (-v, 60 s timed return) with the kernel rebuilt from source with the r5q overlay (timer INTID from boot-arg gic_vtimer) and the loader passing gic_vtimer=19 on r5q. Expected: the kernel keeps running, its 60 s timer fires, it returns with its log." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 35 showed the boot on screen (2026-11-16 ~17:25)." | grep -E '"bundle"|"image"'
