#!/bin/sh
# Test 36: Test 35 (-v, 60 s timed run) with the timer fix: kernel built with the r5q overlay (timer INTID from gic_vtimer=) and the loader passing gic_vtimer=19 (SM8150 virtual timer)
# no per-region clean over the firmware map (R5Q_SKIP_MAP_CLEAN), ABL-style set/way flush and shutdown order
# (R5Q_ABL_SEQUENCE), coherent RAM-ring trace (R5Q_COHERENT_TRACE).
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1  -DR5Q_RESERVED_WINDOW=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -DR5Q_SKIP_MAP_CLEAN=1 -DR5Q_QCOM_VTIMER=19 -DR5Q_XNU_CHECKPOINT=0 -DR5Q_LAB_TIMED=60 "-DR5Q_LAB_CMDLINE=\"-v loader_return_after=60\"" "-DR5Q_TEST_LABEL=\"TEST36\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
