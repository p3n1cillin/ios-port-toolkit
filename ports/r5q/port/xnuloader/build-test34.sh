#!/bin/sh
# Test 34: real XNU with no checkpoint: it boots as far as it can for 25 s (loader_return_after) and returns with its log with Test 31's hand-off, which returned on the phone:
# no per-region clean over the firmware map (R5Q_SKIP_MAP_CLEAN), ABL-style set/way flush and shutdown order
# (R5Q_ABL_SEQUENCE), coherent RAM-ring trace (R5Q_COHERENT_TRACE).
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1  -DR5Q_RESERVED_WINDOW=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -DR5Q_SKIP_MAP_CLEAN=1 -DR5Q_XNU_CHECKPOINT=0 -DR5Q_LAB_TIMED=25 "-DR5Q_TEST_LABEL=\"TEST34\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
