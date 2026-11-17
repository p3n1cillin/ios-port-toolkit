#!/bin/bash
# EMULATION ONLY: boot full iOS 16.5 (donor userland images) in QEMU with a chosen kernel.
# usage: [LOADER=loader.efi] run_qemu_ios.sh KERNEL LABEL SECONDS SHOT_AT...
# The log and screenshots go to port/build/qemu-boot/ios-LABEL*. No network (-nic none).
set -u
KERNEL=$1 LABEL=$2 SECS=$3; shift 3
OUT=/mnt/c/Users/user/source/repos/WIP/S10-Lite-project/work/ios-r5q/port/build/qemu-boot
DONOR=/var/tmp/codex-ios-r5q/work/linux-source
LOG=/var/tmp/codex-ios-r5q/work/qemu-ios-$LABEL.log
cd "$DONOR" || exit 1
KERNEL=$KERNEL LOADER=${LOADER:-} QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd \
	qemu/run-ios-kernel.sh "$SECS" -nic none > "$LOG" 2>&1 &
run=$!
start=$(date +%s)
for at in "$@"; do
	while [ $(( $(date +%s) - start )) -lt "$at" ] && kill -0 $run 2>/dev/null; do sleep 5; done
	FULL=1 bash qemu/screendump.sh "$OUT/ios-$LABEL-${at}s.png" || echo "screendump at ${at}s failed"
done
wait $run
echo "qemu run exit: $?"
cp "$LOG" "$OUT/ios-$LABEL.log"
