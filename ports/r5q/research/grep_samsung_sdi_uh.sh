#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# Read-only: SDI disable path, sec_debug dload default, and where uH/RKP are started.
cd ${R5Q_WORK}/research-src/sm-g770f-kernel || exit 1
echo "== msm-poweroff.c 225-290"
sed -n 225,290p drivers/power/reset/msm-poweroff.c
echo "== sec_debug.c 100-150"
sed -n 100,150p drivers/samsung/debug/sec_debug.c
echo "== uh/rkp entry points"
grep -n "uh_call\|hvc\|smc" arch/arm64/kernel/uh_entry.S | head
grep -n "RKP_START\|RKP_INIT\|RKP_DEFERRED\|#define RKP_" include/linux/rkp.h | head -20
