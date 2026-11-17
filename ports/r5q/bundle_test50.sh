#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/." && pwd)}   # ports/r5q of this repository
set -eu
R=${R5Q_PORT}
cd "$R"
python3 package_test50.py "$R" | grep -E '"sha256"' | head -1
python3 bundle_native_test.py 50 --firmware-number 50 --mode diagnostic \
  --notes "Test 49 (lab loader + R5Q_LAB_USERLAND, root.apfs as md0, full firmware): the user saw 'spurious gic interrupt 0x13' over and over: the kernel came back, but its virtual timer (INTID 19, gic_vtimer=19) stayed enabled in this core's redistributor and the boot chain's CNTV was left enabled and expired; the return stub restores it and unmasks IRQs, so UEFI took a level-interrupt storm and the DEBUG firmware printed it forever (forced restart; its log lost to the cold reset). Test 50 = Test 49 + R5Q_GIC_RESTORE: CNTV IMASK set before the context save, this core's extra PPI/SGI enables disabled and un-pended after the return, both logged." \
  --start-blocked-reason "Built and checked host-side. A hardware run needs the user to authorize it explicitly (one run per authorization). Run after Test 49 (2026-11-16 19:52)....." | grep -E '"bundle"|"image"'
