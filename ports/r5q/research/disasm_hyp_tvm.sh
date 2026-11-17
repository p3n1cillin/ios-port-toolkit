#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Who enables HCR_EL2.TVM / stage 2 in the r5q uH image, and what its sysreg-trap handler does (data only).
cd ${R5Q_WORK}/research-fw || exit 1
D=hyp-seg3.dis
echo "== callers of the TVM-enable routine 0x8576bf74 and TVM-disable 0x8576bf9c"
grep -n -E 'bl\s+0x8576bf74|bl\s+0x8576bf9c|b\s+0x8576bf74' $D | head
echo "== callers of stage-2 enable 0x85790848 and stage-2 disable 0x8579087c"
grep -n -E 'b(l)?\s+0x85790848|b(l)?\s+0x8579087c' $D | head
echo "== references to the boot TVM flag 0x858d59c0 (adrp 0x858d5000 + 0x9c0)"
grep -n -A2 'adrp.*0x858d5000' $D | grep -B1 '#0x9c0\|#2496' | head -20
echo "== TVM handler installed at 0x8576bf58 (first 40 instructions)"
n=$(grep -n "^ *8576bf58:" $D | cut -d: -f1); [ -n "$n" ] && sed -n "$((n-30)),$((n+3))p" $D | cut -f1,3- | cut -c1-80
