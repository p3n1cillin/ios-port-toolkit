#!/bin/bash
R5Q_WORK=${R5Q_WORK:-/var/tmp/codex-ios-r5q/work}   # the Linux-side workspace (docs/SETUP.md)
# build_init.sh (2026-11-20): init and var_store compiled from the userland's source (IOS_WORK/linux-source/userland/src)
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
