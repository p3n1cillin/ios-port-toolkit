#!/bin/sh
# EMULATION ONLY: the port loader for QEMU virt, with the donor's flags and base (0xA1000000).
set -eu
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
  /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
