#!/bin/bash
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}   # ports/r5q of this repository
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Build the QEMU port loader with the ABL cache sequence and run it under the EL2 uH stand-in.
R=${R5Q_PORT}
L=${R5Q_WORK}/port-loader-qemu-abl.efi
cd /tmp && python3 -I $R/build_loader_linux.py $R/port/xnuloader $L --qemu-abl | tail -1
aarch64-linux-gnu-objdump -D -b binary -m aarch64 $L | grep -c 'dc\s*cisw' | sed 's/^/dc cisw instructions in loader: /'
LOADER=$L PREFIX=abl- bash $R/el2-model/run_el2_model.sh 999 5 11
