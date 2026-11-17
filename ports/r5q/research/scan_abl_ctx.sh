#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Context around ABL LinuxLoader's set/way cache ops and SCTLR_EL1 writes (data only).
D=/tmp/abl_pe_2100e8.efi.dis
[ -f $D ] || aarch64-linux-gnu-objdump -D -b binary -m aarch64 ${R5Q_WORK}/abl_pe_2100e8.efi > $D
show() { awk -v n="$1" -v b="$2" -v a="$3" 'NR>=n-b && NR<=n+a' $D | cut -f1,3- | cut -c1-70; echo '   ----'; }
echo "##### set/way operations"
for n in $(grep -n -E 'dc\s*(cisw|csw|isw)' $D | cut -d: -f1); do show $n 14 10; done
echo "##### SCTLR_EL1 writes"
for n in $(grep -n 'msr\s*sctlr_el1' $D | cut -d: -f1); do show $n 6 4; done
