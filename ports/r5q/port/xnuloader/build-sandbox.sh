#!/bin/sh
set -eu
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin \
    -fno-stack-protector -fshort-wchar -mgeneral-regs-only -O1 \
    -Wall -Wextra -Wno-unused-parameter -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
    /base:0xE0000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w 0 /scratch/loader.efi
