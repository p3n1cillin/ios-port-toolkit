#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# build_init.sh (2026-11-20): init and var_store (and, from Test 413, libredwood-media.dylib) compiled from the userland's source (IOS_WORK/linux-source/userland/src)
# into port/build/userland/, for build_root_app.sh to put into the root in place of the donor's prebuilt ones. Run in
# WSL/Linux.
#
# Why: build.sh runs with PREBUILT=1 (no ld64 here), so the root carried prebuilt/userland/init from 2026-11-15, and a
# change to src/init.c never reached the phone. The first such change is /private/var on the flash (var_hfs=1, Test
# 411, wifi/STORAGE-PLAN.md). Same flags as build.sh's own compile; xnu-clang links with lld (16 KB pages), so the files are
# larger than the prebuilt ones.
set -e
R5Q_PORT=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
IOS_WORK=${IOS_WORK:-${R5Q_WORK}}
U=$IOS_WORK/linux-source/userland
OUT=$R5Q_PORT/port/build/userland
command -v xnu-clang >/dev/null || PATH=$PATH:$(dirname "$(find $IOS_WORK -name xnu-clang -type f | head -1)")
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
# (Test 411) lld takes -static but writes a dynamic executable, and process 1 cannot have dyld ("initproc failed to
# start -- Library not loaded: /usr/lib/libSystem.B.dylib"): link with classic rebases and room in the header, then
# macho_static.py makes it static the way ld64 -static does (LC_UNIXTHREAD, no dyld, not PIE)
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000 -Wl,-no_fixup_chains -Wl,-headerpad,0x400"
T=$(mktemp -d)
mkdir -p "$OUT"
$CC -I$U/libmini -c $U/libmini/mini.c -o $T/mini.o
$CC -c $U/libmini/start.S -o $T/start.o
for prog in init var_store; do
	$CC -I$U/libmini $LD -o $T/$prog $U/src/$prog.c $T/mini.o $T/start.o
	python3 $R5Q_PORT/pipeline/macho_static.py $T/$prog $OUT/$prog
	chmod 755 $OUT/$prog
	echo "build_init: $OUT/$prog ($(stat -c %s $OUT/$prog) bytes)"
done
rm -rf "$T"
# (Test 413) libredwood-media.dylib -- mediaserverd's capture device and software ISP (src/camera_device.c,
# src/camera_isp.c) -- from source too: the root carried prebuilt/userland's from 2026-11-15, and the exposure fixes
# for the back camera (camera_isp.c, isp_ae) would never have reached it. The SDK's clang and lld, as build.sh's $DYN.
SDK=${SDK:-$IOS_WORK/sdks-source/iPhoneOS16.5.sdk}
OH_INC=${OH_INC:-$IOS_WORK/openh264-ios/src/codec/api/wels}
OH_LIB=${OH_LIB:-$IOS_WORK/openh264-ios/libopenh264_enc.a}
# (Test 451) without OpenH264 or the donor's vt_h264_enc.c, say what is missing and keep init/var_store (set -e had
# stopped here, and build_root_app.sh then quietly kept an older libredwood-media.dylib)
missing=""
[ -f "$OH_LIB" ] || missing="$missing $OH_LIB (bash pipeline/build_openh264.sh makes it)"
[ -d "$OH_INC" ] || missing="$missing $OH_INC (the same)"
for s in inject_media.c camera_device.c camera_isp.c vt_h264_enc.c; do
	[ -f "$U/src/$s" ] || missing="$missing $U/src/$s (the donor's userland, with userland-patches/ applied)"
done
if [ -n "$missing" ]; then
	echo "build_init: libredwood-media.dylib NOT built; missing:$missing"
	echo "build_init: build_root_app.sh will use $OUT/libredwood-media.dylib if an older one is there, else the prebuilt"
	exit 0
fi
clang-target arm64-apple-ios16.5 -isysroot $SDK -fuse-ld=lld -O2 -dynamiclib -install_name /redwood/lib/libredwood-media.dylib -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox -lc++ -lobjc -I$U/src -I$OH_INC -o $OUT/libredwood-media.dylib $U/src/inject_media.c $U/src/camera_device.c $U/src/camera_isp.c $U/src/vt_h264_enc.c $OH_LIB
echo "build_init: $OUT/libredwood-media.dylib ($(stat -c %s $OUT/libredwood-media.dylib) bytes)"
