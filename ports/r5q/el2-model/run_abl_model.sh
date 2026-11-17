#!/bin/bash
# Build the QEMU port loader with the ABL cache sequence and run it under the EL2 uH stand-in.
R=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q
L=/var/tmp/codex-ios-r5q/work/port-loader-qemu-abl.efi
cd /tmp && python3 -I $R/build_loader_linux.py $R/port/xnuloader $L --qemu-abl | tail -1
aarch64-linux-gnu-objdump -D -b binary -m aarch64 $L | grep -c 'dc\s*cisw' | sed 's/^/dc cisw instructions in loader: /'
LOADER=$L PREFIX=abl- bash $R/el2-model/run_el2_model.sh 999 5 11
