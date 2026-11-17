#!/bin/sh
# Test 33: real XNU entry to checkpoint 5 (just after the kernel enables its own MMU and caches in start.s) with Test 31's hand-off, which returned on the phone:
# no per-region clean over the firmware map (R5Q_SKIP_MAP_CLEAN), ABL-style set/way flush and shutdown order
# (R5Q_ABL_SEQUENCE), coherent RAM-ring trace (R5Q_COHERENT_TRACE).
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1  -DR5Q_RESERVED_WINDOW=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -DR5Q_SKIP_MAP_CLEAN=1 -DR5Q_XNU_CHECKPOINT=5 "-DR5Q_TEST_LABEL=\"TEST33\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
