#!/bin/sh
set -eu
# EMULATION ONLY: Test 15 profile stopping at kernel checkpoint $R5Q_CP. Never bundle for hardware.
case "$R5Q_CP" in [0-9]|[0-9][0-9]) ;; *) echo "bad checkpoint" >&2; exit 2;; esac
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1 \
 -DR5Q_TEST15=1 -DR5Q_RESERVED_WINDOW=1 -DR5Q_XNU_CHECKPOINT="$R5Q_CP" \
 -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
 -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter \
 -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
 /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
