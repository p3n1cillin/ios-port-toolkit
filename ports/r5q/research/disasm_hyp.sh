#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Static disassembly of the stock r5q hypervisor image (data only; never executed).
set -eu
cd ${R5Q_WORK}/research-fw
OD=aarch64-linux-gnu-objdump
seg() { dd if=hyp.mbn of="$1" bs=1 skip=$(( $2 )) count=$(( $3 )) status=none; }
seg hyp-seg0.bin 0x3000 0xd59c
seg hyp-seg3.bin 0x12940 0x74388
$OD -D -b binary -m aarch64 --adjust-vma=0x85700000 hyp-seg0.bin > hyp-seg0.dis
$OD -D -b binary -m aarch64 --adjust-vma=0x85740000 hyp-seg3.bin > hyp-seg3.dis
for f in hyp-seg0.dis hyp-seg3.dis; do
	echo "=== $f: EL2 control-register writes"
	grep -n -E 'msr\s+(hcr_el2|sctlr_el2|vtcr_el2|vttbr_el2|mdcr_el2|cptr_el2|hstr_el2|tcr_el2|vbar_el2)' "$f" | head -40
done
echo "=== strings hinting at identity"
strings -n 6 hyp.mbn | grep -i -E 'uh|rkp|qhee|hyp|samsung|version|QC_IMAGE|OEM_IMAGE' | head -25
