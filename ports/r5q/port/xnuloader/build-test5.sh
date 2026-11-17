#!/bin/sh
set -eu
clang --target=aarch64-unknown-windows -DR5Q_FIRST_TEST=1 -DR5Q_LAB_TRACE=1 \
    -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter \
    -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
    /base:0xE0000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
