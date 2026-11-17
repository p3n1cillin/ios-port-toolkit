#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Print disassembly context around given hypervisor addresses (data only).
cd ${R5Q_WORK}/research-fw || exit 1
ctx() { # ctx ADDR BEFORE AFTER
	local n; n=$(grep -n "^ *$1:" hyp-seg3.dis | cut -d: -f1)
	[ -n "$n" ] || { echo "no $1"; return; }
	echo "---- around $1"
	sed -n "$((n-$2)),$((n+$3))p" hyp-seg3.dis | cut -f1,3- | cut -c1-80
}
ctx 857414ec 22 4
ctx 85741518 8 4
ctx 85741bb4 16 4
ctx 8576bf94 14 6
ctx 85790848 14 40
# the literal pool words the boot path loads its flag pointer from
echo "---- literal 0x85740370 (flag pointer for HCR.TVM at boot):"
od -An -tx8 -j $((0x370)) -N 8 hyp-seg3.bin
