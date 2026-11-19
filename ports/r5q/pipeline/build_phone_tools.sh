#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# build_phone_tools.sh: every on-phone test tool (wifi/*.c, *.m) built with the iOS SDK into port/build/phone-tools/,
# for `python phone.py tools` to put them all into /var/r5q/bin at once (after a crash /var may not have kept them).
# Run in WSL/Linux. (2026-11-19: before this, each tool was built and put by hand, again after every crash.)
set -e
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
IOS_WORK=${IOS_WORK:-${R5Q_WORK}}
SDK=${SDK:-$IOS_WORK/sdks-source/iPhoneOS16.5.sdk}
W=$R5Q_PORT/wifi
OUT=$R5Q_PORT/port/build/phone-tools
mkdir -p "$OUT"
CC="clang -target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O1 -Wno-everything"
FW="-framework Foundation"
# name: source and extra flags
build() {
	local name=$1 src=$2; shift 2
	if $CC "$@" -o "$OUT/$name" "$W/$src" 2>"$OUT/.$name.err"; then
		printf '  %-12s ok\n' "$name"
	else
		printf '  %-12s FAILED: %s\n' "$name" "$(head -1 "$OUT/.$name.err")"
	fi
	rm -f "$OUT/.$name.err"
}
build cm_test     cm_test.m     -fobjc-arc $FW -framework CoreMotion
build cm_rate     cm_rate.m     -fobjc-arc $FW -framework CoreMotion
build cl_test     cl_test.m     -fobjc-arc $FW -framework CoreLocation
build openurl     openurl.m     -fobjc-arc $FW -framework MobileCoreServices
build vt_test     vt_test.m     -fobjc-arc $FW -framework AVFoundation -framework CoreMedia -framework CoreVideo \
	-framework VideoToolbox -framework QuartzCore
build avplay_test avplay_test.m -fobjc-arc $FW -framework AVFoundation -framework AudioToolbox
build bt_test     bt_test.m     -fobjc-arc $FW -framework CoreBluetooth
build cfpref      cfpref.c      -framework CoreFoundation
build sc_name     sc_name.c     -framework CoreFoundation -framework SystemConfiguration
build fps_test    fps_test.c    -framework QuartzCore
build orient_test orient_test.c
build r5qfs       r5qfs.c
build r5q_sysctl  r5q_sysctl.c
build r5q_reboot  r5q_reboot.c
build magrec      magrec.c
build sym_probe   sym_probe.c
echo "phone tools in $OUT"
