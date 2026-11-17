#!/bin/sh
# Test 51: the iOS system volume (system.apfs, 2.4 GB) arrives over the USB RAM drive and lies at the resident address 0xFC000000 (md0, the root); root.apfs is md1. Test 50 loader + R5Q_GIC_REENABLE (UEFI's own PPIs the kernel turned off come back on), 240 s timed return.
# per-region map clean, gic_vtimer=19, timed return with the kernel log in the RAM ring) with R5Q_LAB_USERLAND:
# window = largest free block (0xF4000000, 6 GiB) instead of the lab's 256 MiB, the payload's ramdisk (root.apfs)
# as the root, 90 s timed return without -v (the verbose console scrolls 10 MB per line).
set -eu
clang --target=aarch64-unknown-windows -DR5Q_XNU_TEST=1 -DR5Q_LAB_TRACE=1 -DR5Q_COHERENT_TRACE=1 -DR5Q_ABL_SEQUENCE=1 -DR5Q_SKIP_MAP_CLEAN=1 -DR5Q_QCOM_VTIMER=19 -DR5Q_XNU_CHECKPOINT=0 -DR5Q_LAB_TIMED=240 -DR5Q_LAB_USERLAND=1 -DR5Q_GIC_RESTORE=1 -DR5Q_GIC_REENABLE=1 "-DR5Q_LAB_CMDLINE=\"loader_return_after=240\"" "-DR5Q_TEST_LABEL=\"TEST51\""  -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter  -Wno-unused-function -c /src/loader.c -o /scratch/loader.obj
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib  /base:0x99000000 /out:/scratch/loader.efi /scratch/loader.obj
base64 -w0 /scratch/loader.efi
