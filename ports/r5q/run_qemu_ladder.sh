#!/bin/bash
# EMULATION ONLY: walk every donor kernel checkpoint under QEMU for one kernel.
# usage: run_qemu_ladder.sh ROOT KERNEL LABEL
# Writes port/build/qemu-boot/LABEL-cpN/ per checkpoint and port/build/qemu-boot/LABEL-ladder.txt.
set -u
ROOT=$1 KERNEL=$2 LABEL=$3
OUT=$ROOT/port/build/qemu-boot/$LABEL-ladder.txt
: > "$OUT"
for cp in 1 2 3 4 5 10 11 30 31 32 33 34 35 12 40 41 42 43 44 13 14 15 16 17 18 19 20 21; do
	python3 -I "$ROOT/run_qemu_kernel.py" "$ROOT" "$KERNEL" "$LABEL-cp$cp" 40 "$cp" > /dev/null 2>&1
	python3 -I - "$ROOT/port/build/qemu-boot/$LABEL-cp$cp/receipt.json" "$cp" >> "$OUT" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1])); m = r['markers']
print(f"checkpoint {int(sys.argv[2]):2d}: {'OK  ' if m['stopped_at_requested_checkpoint'] else 'FAIL'} "
      f"{r['seconds']:5.1f}s{'  early-exception' if m['early_exception'] else ''}")
EOF
done
cat "$OUT"
