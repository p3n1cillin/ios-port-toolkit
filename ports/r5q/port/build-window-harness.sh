#!/bin/sh
set -eu
clang --target=aarch64-unknown-windows -DMDE_CPU_AARCH64 -I/mde -I/mde/AArch64 -I/src \
 -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar -mgeneral-regs-only \
 -O1 -Wall -Wextra -c /src/window_harness.c -o /scratch/harness.obj
lld-link /subsystem:efi_application /entry:test_entry /nodefaultlib \
 /base:0x9A000000 /out:/scratch/loader.efi /scratch/harness.obj
base64 -w0 /scratch/loader.efi
