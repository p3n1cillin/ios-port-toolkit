#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# build_openh264.sh (2026-11-24, Test 451): Cisco's OpenH264 encoder as a static arm64 iOS library, for
# libredwood-media.dylib's VideoToolbox 'avc1' encoder (vt_h264_enc.c, Tests 431-434). Run in WSL/Linux.
#
# Why: build_init.sh links $OH_LIB (libopenh264_enc.a) and includes $OH_INC (codec/api/wels); Test 431 built them by
# hand. This makes them again from a clean checkout of the same release: the encoder, common and processing sources
# and their aarch64 NEON assembly (the 81 members Test 431's archive has), with OpenH264's own iOS flags
# (build/platform-ios.mk: -DAPPLE_IOS; build/arch.mk: -DHAVE_NEON_AARCH64). No decoder, no console tools.
#
#   bash pipeline/build_openh264.sh                 # into $IOS_WORK/openh264-ios (src/, build/, libopenh264_enc.a)
#   OH_DIR=/tmp/oh bash pipeline/build_openh264.sh  # somewhere else
#
# OpenH264 is BSD-2-Clause (its LICENSE travels with src/). Cisco's binary releases carry a patent licence for H.264
# that a build from source does not: see https://www.openh264.org/ before you distribute what you build.
set -e
IOS_WORK=${IOS_WORK:-${R5Q_WORK}}
SDK=${SDK:-$IOS_WORK/sdks-source/iPhoneOS16.5.sdk}
OH_DIR=${OH_DIR:-$IOS_WORK/openh264-ios}
OH_TAG=${OH_TAG:-v2.4.1}
[ -d "$SDK" ] || { echo "build_openh264: no iOS SDK at $SDK (set SDK=)"; exit 1; }
mkdir -p "$OH_DIR"
if [ ! -d "$OH_DIR/src/codec" ]; then
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$OH_TAG" https://github.com/cisco/openh264.git "$OH_DIR/src"
fi
S=$OH_DIR/src
B=$OH_DIR/build
mkdir -p "$B/gen" "$B/obj"
echo "#define VERSION_NUMBER \"${OH_TAG#v}\"" > "$B/gen/version_gen.h"
INC="-I$B/gen -I$S/codec/api/wels -I$S/codec/common/inc -I$S/codec/common/arm64 -I$S/codec/encoder/core/inc \
-I$S/codec/encoder/plus/inc -I$S/codec/processing/interface -I$S/codec/processing/src/common"
for d in "$S"/codec/processing/src/*/; do INC="$INC -I$d"; done
FLAGS="-target arm64-apple-ios16.5 -isysroot $SDK -O3 -DAPPLE_IOS -DHAVE_NEON_AARCH64 -DNDEBUG -fPIC"
SRCS=$(cd "$S" && ls codec/common/src/*.cpp codec/common/arm64/*.S codec/encoder/core/src/*.cpp \
	codec/encoder/core/arm64/*.S codec/encoder/plus/src/welsEncoderExt.cpp codec/processing/src/*/*.cpp \
	codec/processing/src/arm64/*.S)
n=0
for f in $SRCS; do
	o=$B/obj/$(echo "$f" | tr '/' '_').o
	case $f in
	*.cpp) clang++ $FLAGS -std=c++11 -fno-exceptions -fno-rtti -Wno-everything $INC -c "$S/$f" -o "$o" ;;
	*.S) clang $FLAGS $INC -c "$S/$f" -o "$o" ;;
	esac
	n=$((n + 1))
done
rm -f "$OH_DIR/libopenh264_enc.a"
llvm-ar rcs "$OH_DIR/libopenh264_enc.a" "$B"/obj/*.o
echo "build_openh264: $OH_DIR/libopenh264_enc.a ($n objects, $(stat -c %s "$OH_DIR/libopenh264_enc.a") bytes);" \
	"for build_init.sh: OH_LIB=$OH_DIR/libopenh264_enc.a OH_INC=$S/codec/api/wels"
