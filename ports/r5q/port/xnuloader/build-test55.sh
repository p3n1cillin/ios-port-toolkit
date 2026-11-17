#!/bin/sh
# Test 55: display resync run (firmware: one clean frame with autorefresh paused, no more kicks, 15 s calibration pattern). Loader: Test 50 (root.apfs only, 90 s) + R5Q_GIC_REENABLE.
# per-region map clean, gic_vtimer=19, timed return with the kernel log in the RAM ring) with R5Q_LAB_USERLAND:
# window = largest free block (0xF4000000, 6 GiB) instead of the lab's 256 MiB, the payload's ramdisk (root.apfs)
# as the root, 90 s timed return without -v (the verbose console scrolls 10 MB per line).
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -DR5Q_SKIP_MAP_CLEAN=1 -DR5Q_QCOM_VTIMER=19 -DR5Q_XNU_CHECKPOINT=0 -DR5Q_LAB_TIMED=90 -DR5Q_LAB_USERLAND=1 -DR5Q_GIC_RESTORE=1 -DR5Q_GIC_REENABLE=1 "-DR5Q_LAB_CMDLINE=\"loader_return_after=90\"" "-DR5Q_TEST_LABEL=\"TEST55\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
