#!/bin/sh
# Tests 19-23: Test 18's return-selftest profile plus stub stage tags, ending (STAGE 1-4) in the
# firmware's PS_HOLD reset after a chosen step, or (STAGE 0) the full return.
set -eu
case "$R5Q_N" in 19|2[0-9]) ;; *) echo "bad test number" >&2; exit 2;; esac
case "$R5Q_STAGE" in [0-5]) ;; *) echo "bad stage" >&2; exit 2;; esac
RESET=""
[ "$R5Q_STAGE" = 0 ] || RESET="-DR5Q_RESET_STAGE=$R5Q_STAGE"
# Tests 25-28 arm the APSS watchdog before the MMU-off step so a hang reboots by itself.
WDT=""
case "$R5Q_N" in 2[5-8]) WDT="-DR5Q_ARM_WDT=1";; esac
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1 \
 -DR5Q_TEST18=1 -DR5Q_RESERVED_WINDOW=1 -DR5Q_RETURN_SELFTEST=1 -DR5Q_COHERENT_TRACE=1 \
 -DR5Q_STUB_STAGES=1 "-DR5Q_TEST_LABEL=\"TEST$R5Q_N\"" $RESET $WDT \
 -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
 -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter \
 -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
 /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
