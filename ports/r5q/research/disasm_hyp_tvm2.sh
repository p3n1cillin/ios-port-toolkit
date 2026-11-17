#!/bin/bash
# Trace what reaches the TVM enable (0x8576f9ac) and the boot TVM flag writer (0x85743d50). Data only.
cd /var/tmp/codex-ios-r5q/work/research-fw || exit 1
D=hyp-seg3.dis
at() { grep -n "^ *$1:" $D | cut -d: -f1; }
show() { local n; n=$(at "$1"); [ -n "$n" ] && sed -n "$((n-$2)),$((n+$3))p" $D | cut -f1,3- | cut -c1-90; }
# function start: walk back to the nearest "stp x29, x30, [sp, #-..]!" before the call site
fstart() { local n; n=$(at "$1"); awk -v n="$n" 'NR<=n && /stp\tx29, x30, \[sp, #-/ {s=$1} NR==n {print s; exit}' $D | tr -d ':'; }
for site in 8576f9ac 8576f6d4; do
	f=$(fstart $site); echo "== call site $site is in function starting at $f"; show $site 12 4
	echo "-- callers of $f"; grep -n -E "bl\s+0x$f\b" $D | head -5
done
echo "== the boot TVM flag writer around 0x85743d50"
show 85743d50 14 10
echo "== stage-2 dispatcher around 0x8574b488"
show 8574b488 24 12
