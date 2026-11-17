#!/bin/sh
# EMULATION ONLY: the production (non-lab) loader for the r5q virtual twin's iOS mode (twin/run_twin.py ios).
# Identical to the phone's production profile except R5Q_TWIN: the r5q platform gets the twin's PL011 UART.
# Never bundle this for hardware.
set -eu
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -DR5Q_TWIN=1 -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0x9B4DC000 \
  /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
