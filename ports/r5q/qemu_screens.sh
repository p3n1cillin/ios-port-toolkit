#!/bin/bash
# Save the running QEMU's screen now and then every STEP seconds, COUNT times (EMULATION ONLY).
# usage: qemu_screens.sh PREFIX STEP COUNT
cd /var/tmp/codex-ios-r5q/work/linux-source || exit 1
prefix=$1 step=$2 count=$3
for i in $(seq 1 "$count"); do
	FULL=1 bash qemu/screendump.sh "$prefix-$i.png" || echo "screendump $i failed"
	[ "$i" -lt "$count" ] && sleep "$step"
done
